#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <bsd/sys/queue.h>

#ifdef G_OS_WIN32
#include <windows.h>
#endif

#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>

#include <Scintilla.h>

#include "sciteco.h"
#include "interface.h"
#include "undo.h"
#include "parser.h"
#include "expressions.h"
#include "qbuffers.h"

namespace States {
	StateEditFile		editfile;
	StateSaveFile		savefile;

	StateEQCommand		eqcommand;
	StateLoadQReg		loadqreg;
	StateCtlUCommand	ctlucommand;
	StateSetQRegString	setqregstring;
	StateGetQRegInteger	getqreginteger;
	StateSetQRegInteger	setqreginteger;
	StateIncreaseQReg	increaseqreg;
	StateMacro		macro;
	StateCopyToQReg		copytoqreg;
}

Ring ring;
QRegisterTable qregisters;

static QRegister *register_argument = NULL;

/* FIXME: clean up current_save_dot() usage */
static inline void
current_save_dot(void)
{
	gint dot = interface.ssm(SCI_GETCURRENTPOS);

	if (ring.current)
		ring.current->dot = dot;
	else if (qregisters.current)
		qregisters.current->dot = dot;
}

static inline void
current_edit(void)
{
	if (ring.current)
		ring.current->edit();
	else if (qregisters.current)
		qregisters.current->edit();
}

void
QRegister::set_string(const gchar *str)
{
	edit();
	dot = 0;

	interface.ssm(SCI_BEGINUNDOACTION);
	interface.ssm(SCI_SETTEXT, 0, (sptr_t)str);
	interface.ssm(SCI_ENDUNDOACTION);

	current_edit();
}

void
QRegister::undo_set_string(void)
{
	current_save_dot();
	if (ring.current)
		ring.current->undo_edit();
	else if (qregisters.current)
		qregisters.current->undo_edit();

	undo.push_var<gint>(dot);
	undo.push_msg(SCI_UNDO);

	undo_edit();
}

gchar *
QRegister::get_string(void)
{
	gint size;
	gchar *str;

	current_save_dot();
	edit();

	size = interface.ssm(SCI_GETLENGTH) + 1;
	str = (gchar *)g_malloc(size);
	interface.ssm(SCI_GETTEXT, size, (sptr_t)str);

	current_edit();

	return str;
}

bool
QRegister::load(const gchar *filename)
{
	gchar *contents;
	gsize size;

	current_save_dot();
	edit();
	dot = 0;

	/* FIXME: prevent excessive allocations by reading file into buffer */
	if (!g_file_get_contents(filename, &contents, &size, NULL))
		return false;

	interface.ssm(SCI_BEGINUNDOACTION);
	interface.ssm(SCI_CLEARALL);
	interface.ssm(SCI_APPENDTEXT, size, (sptr_t)contents);
	interface.ssm(SCI_ENDUNDOACTION);

	g_free(contents);

	current_edit();

	return true;
}

void
QRegisterTable::initialize(void)
{
	/* general purpose registers */
	for (gchar q = 'A'; q <= 'Z'; q++)
		initialize_register((gchar []){q, '\0'});
	for (gchar q = '0'; q <= '9'; q++)
		initialize_register((gchar []){q, '\0'});

	/* search string and status register */
	initialize_register("_");
}

void
QRegisterTable::edit(QRegister *reg)
{
	current_save_dot();
	reg->edit();

	ring.current = NULL;
	current = reg;
}

bool
Buffer::load(const gchar *filename)
{
	gchar *contents;
	gsize size;

	edit();
	interface.ssm(SCI_CLEARALL);

	/* FIXME: prevent excessive allocations by reading file into buffer */
	if (!g_file_get_contents(filename, &contents, &size, NULL))
		return false;
	interface.ssm(SCI_APPENDTEXT, size, (sptr_t)contents);
	g_free(contents);

	interface.ssm(SCI_GOTOPOS, 0);
	interface.ssm(SCI_SETSAVEPOINT);

	set_filename(filename);

	return true;
}

void
Buffer::close(void)
{
	LIST_REMOVE(this, buffers);

	if (filename)
		interface.msg(Interface::MSG_INFO,
			      "Removed file \"%s\" from the ring",
			      filename);
	else
		interface.msg(Interface::MSG_INFO,
			      "Removed unnamed file from the ring.");
}

void
Ring::UndoTokenEdit::run(void)
{
	/*
	 * assumes that buffer still has correct prev/next
	 * pointers
	 */
	*buffer->buffers.le_prev = buffer;
	if (buffer->next())
		buffer->next()->buffers.le_prev = &buffer->next();

	ring->current = buffer;
	buffer->edit();
	buffer = NULL;
}

Buffer *
Ring::find(const gchar *filename)
{
	gchar *resolved = get_absolute_path(filename);
	Buffer *cur;

	LIST_FOREACH(cur, &head, buffers)
		if (!g_strcmp0(cur->filename, resolved))
			break;

	g_free(resolved);
	return cur;
}

bool
Ring::edit(const gchar *filename)
{
	bool new_in_ring = false;
	Buffer *buffer = find(filename);

	current_save_dot();

	if (buffer) {
		buffer->edit();
	} else {
		new_in_ring = true;

		buffer = new Buffer();
		LIST_INSERT_HEAD(&head, buffer, buffers);

		if (g_file_test(filename, G_FILE_TEST_IS_REGULAR)) {
			buffer->load(filename);

			interface.msg(Interface::MSG_INFO,
				      "Added file \"%s\" to ring", filename);
		} else {
			buffer->edit();
			buffer->set_filename(filename);

			if (filename)
				interface.msg(Interface::MSG_INFO,
					      "Added new file \"%s\" to ring",
					      filename);
			else
				interface.msg(Interface::MSG_INFO,
					      "Added new unnamed file to ring.");
		}
	}

	qregisters.current = NULL;
	current = buffer;

	return new_in_ring;
}

#if 0

/*
 * TODO: on UNIX it may be better to open() the current file, unlink() it
 * and keep the file descriptor in the UndoToken.
 * When the operation is undone, the file descriptor's contents are written to
 * the file (which should be efficient enough because it is written to the same
 * filesystem). This way we could avoid messing around with save point files.
 */

#else

class UndoTokenRestoreSavePoint : public UndoToken {
	gchar	*savepoint;
	Buffer	*buffer;

public:
#ifdef G_OS_WIN32
	DWORD attributes;
#endif

	UndoTokenRestoreSavePoint(gchar *_savepoint, Buffer *_buffer)
				 : savepoint(_savepoint), buffer(_buffer) {}
	~UndoTokenRestoreSavePoint()
	{
		if (savepoint)
			g_unlink(savepoint);
		g_free(savepoint);
		buffer->savepoint_id--;
	}

	void
	run(void)
	{
		if (!g_rename(savepoint, buffer->filename)) {
			g_free(savepoint);
			savepoint = NULL;
#ifdef G_OS_WIN32
			SetFileAttributes((LPCTSTR)buffer->filename,
					  attributes);
#endif
		} else {
			interface.msg(Interface::MSG_WARNING,
				      "Unable to restore save point file \"%s\"",
				      savepoint);
		}
	}
};

static inline void
make_savepoint(Buffer *buffer)
{
	gchar *dirname, *basename, *savepoint;
	gchar savepoint_basename[FILENAME_MAX];

	basename = g_path_get_basename(buffer->filename);
	g_snprintf(savepoint_basename, sizeof(savepoint_basename),
		   ".teco-%s-%d", basename, buffer->savepoint_id);
	g_free(basename);
	dirname = g_path_get_dirname(buffer->filename);
	savepoint = g_build_filename(dirname, savepoint_basename, NULL);
	g_free(dirname);

	if (!g_rename(buffer->filename, savepoint)) {
		UndoTokenRestoreSavePoint *token;

		buffer->savepoint_id++;
		token = new UndoTokenRestoreSavePoint(savepoint, buffer);
#ifdef G_OS_WIN32
		token->attributes = GetFileAttributes((LPCTSTR)savepoint);
		if (token->attributes != INVALID_FILE_ATTRIBUTES)
			SetFileAttributes((LPCTSTR)savepoint,
					  attrs | FILE_ATTRIBUTE_HIDDEN);
#endif
		undo.push(token);
	} else {
		interface.msg(Interface::MSG_WARNING,
			      "Unable to create save point file \"%s\"",
			      savepoint);
		g_free(savepoint);
	}
}

#endif /* !G_OS_UNIX */

bool
Ring::save(const gchar *filename)
{
	const gchar *buffer;
	gssize size;

	if (!current)
		return false;

	if (!filename)
		filename = current->filename;
	if (!filename)
		return false;

	if (undo.enabled) {
		if (current->filename &&
		    g_file_test(current->filename, G_FILE_TEST_IS_REGULAR))
			make_savepoint(current);
		else
			undo.push(new UndoTokenRemoveFile(filename));
	}

	buffer = (const gchar *)interface.ssm(SCI_GETCHARACTERPOINTER);
	size = interface.ssm(SCI_GETLENGTH);

	if (!g_file_set_contents(filename, buffer, size, NULL))
		return false;

	/*
	 * FIXME: necessary also if the filename was not specified but the file
	 * is (was) new, in order to canonicalize the filename.
	 * May be circumvented by cananonicalizing without requiring the file
	 * name to exist (like readlink -f)
	 */
	//if (filename) {
	undo.push_str(current->filename);
	current->set_filename(filename);
	//}

	return true;
}

void
Ring::close(void)
{
	Buffer *buffer = current;

	buffer->dot = interface.ssm(SCI_GETCURRENTPOS);
	buffer->close();
	current = buffer->next() ? : first();
	/* transfer responsibility to UndoToken object */
	undo.push(new UndoTokenEdit(this, buffer));

	if (current) {
		current->edit();
	} else {
		edit(NULL);
		undo_close();
	}
}

Ring::~Ring()
{
	Buffer *buffer, *next;

	LIST_FOREACH_SAFE(buffer, &head, buffers, next)
		delete buffer;
}

/*
 * Auxiliary functions
 */
#ifdef G_OS_UNIX

gchar *
get_absolute_path(const gchar *path)
{
	gchar buf[PATH_MAX];
	gchar *resolved;

	if (!path)
		return NULL;

	if (!realpath(path, buf)) {
		if (g_path_is_absolute(path)) {
			resolved = g_strdup(path);
		} else {
			gchar *cwd = g_get_current_dir();
			resolved = g_build_filename(cwd, path, NULL);
			g_free(cwd);
		}
	} else {
		resolved = g_strdup(buf);
	}

	return resolved;
}

#else

gchar *
get_absolute_path(const gchar *path)
{
	/* FIXME: see Unix implementation */
	return path ? g_file_read_link(path, NULL) : NULL;
}

#endif

/*
 * Command states
 */

void
StateEditFile::do_edit(const gchar *filename)
{
	if (ring.current)
		ring.undo_edit();
	else /* qregisters.current != NULL */
		qregisters.undo_edit();
	if (ring.edit(filename))
		ring.undo_close();
}

void
StateEditFile::initial(void)
{
	gint64 id = expressions.pop_num_calc(1, -1);

	if (id == 0) {
		for (Buffer *cur = ring.first(); cur; cur = cur->next())
			interface.popup_add_filename(Interface::POPUP_FILE,
						     cur->filename ? : "(Unnamed)",
						     cur == ring.current);

		interface.popup_show();
	}
}

State *
StateEditFile::done(const gchar *str)
{
	BEGIN_EXEC(&States::start);

	if (is_glob_pattern(str)) {
		gchar *dirname;
		GDir *dir;

		dirname = g_path_get_dirname(str);
		dir = g_dir_open(dirname, 0, NULL);

		if (dir) {
			const gchar *basename;
			GPatternSpec *pattern;

			basename = g_path_get_basename(str);
			pattern = g_pattern_spec_new(basename);
			g_free((gchar *)basename);

			while ((basename = g_dir_read_name(dir))) {
				if (g_pattern_match_string(pattern, basename)) {
					gchar *filename;

					filename = g_build_filename(dirname,
								    basename,
								    NULL);
					do_edit(filename);
					g_free(filename);
				}
			}

			g_pattern_spec_free(pattern);
			g_dir_close(dir);
		}

		g_free(dirname);
	} else {
		do_edit(*str ? str : NULL);
	}

	return &States::start;
}

State *
StateSaveFile::done(const gchar *str)
{
	BEGIN_EXEC(&States::start);

	if (!ring.save(*str ? str : NULL))
		return NULL;

	return &States::start;
}

State *
StateEQCommand::got_register(QRegister *reg)
{
	BEGIN_EXEC(&States::loadqreg);
	register_argument = reg;
	return &States::loadqreg;
}

State *
StateLoadQReg::done(const gchar *str)
{
	BEGIN_EXEC(&States::start);

	if (*str) {
		undo.push_var<gint>(register_argument->dot);
		undo.push_msg(SCI_UNDO);
		register_argument->load(str);
	} else {
		if (ring.current)
			ring.undo_edit();
		else /* qregisters.current != NULL */
			qregisters.undo_edit();
		qregisters.edit(register_argument);
	}

	return &States::start;
}

State *
StateCtlUCommand::got_register(QRegister *reg)
{
	BEGIN_EXEC(&States::setqregstring);
	register_argument = reg;
	return &States::setqregstring;
}

State *
StateSetQRegString::done(const gchar *str)
{
	BEGIN_EXEC(&States::start);

	register_argument->undo_set_string();
	register_argument->set_string(str);

	return &States::start;
}

State *
StateGetQRegInteger::got_register(QRegister *reg)
{
	BEGIN_EXEC(&States::start);

	expressions.eval();
	expressions.push(reg->integer);

	return &States::start;
}

State *
StateSetQRegInteger::got_register(QRegister *reg)
{
	BEGIN_EXEC(&States::start);

	undo.push_var<gint64>(reg->integer);
	reg->integer = expressions.pop_num_calc();

	return &States::start;
}

State *
StateIncreaseQReg::got_register(QRegister *reg)
{
	BEGIN_EXEC(&States::start);

	undo.push_var<gint64>(reg->integer);
	reg->integer += expressions.pop_num_calc();
	expressions.push(reg->integer);

	return &States::start;
}

State *
StateMacro::got_register(QRegister *reg)
{
	gint pc = macro_pc;
	gchar *str;
	bool rc;

	BEGIN_EXEC(&States::start);

	/*
	 * need this to fixup state on rubout: state machine emits undo token
	 * resetting state to StateMacro, but the macro executed also emitted
	 * undo tokens resetting the state to StateStart
	 */
	undo.push_var<State*>(States::current);
	States::current = &States::start;

	macro_pc = 0;
	str = reg->get_string();
	rc = macro_execute(str);
	g_free(str);
	macro_pc = pc;
	States::current = this;
	if (!rc)
		return NULL;

	return &States::start;
}

State *
StateCopyToQReg::got_register(QRegister *reg)
{
	gint64 from, len;
	Sci_TextRange tr;

	BEGIN_EXEC(&States::start);
	expressions.eval();

	if (expressions.args() <= 1) {
		from = interface.ssm(SCI_GETCURRENTPOS);
		sptr_t line = interface.ssm(SCI_LINEFROMPOSITION, from) +
			      expressions.pop_num_calc();
		len = interface.ssm(SCI_POSITIONFROMLINE, line) - from;

		if (len < 0) {
			from += len;
			len *= -1;
		}
	} else {
		gint64 to = expressions.pop_num();
		from = expressions.pop_num();
		len = to - from;
	}

	tr.chrg.cpMin = from;
	tr.chrg.cpMax = from + len;
	tr.lpstrText = (char *)g_malloc(len + 1);
	interface.ssm(SCI_GETTEXTRANGE, 0, (sptr_t)&tr);

	reg->undo_set_string();
	reg->set_string(tr.lpstrText);
	g_free(tr.lpstrText);

	return &States::start;
}
