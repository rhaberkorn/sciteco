#include <bsd/sys/queue.h>
#include <string.h>

#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>

#include "gtk-info-popup.h"

#include <Scintilla.h>

#include "sciteco.h"
#include "undo.h"
#include "parser.h"
#include "expressions.h"
#include "qbuffers.h"

namespace States {
	StateFile		file;
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
	gint dot = editor_msg(SCI_GETCURRENTPOS);

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

	editor_msg(SCI_BEGINUNDOACTION);
	editor_msg(SCI_SETTEXT, 0, (sptr_t)str);
	editor_msg(SCI_ENDUNDOACTION);

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

	size = editor_msg(SCI_GETLENGTH) + 1;
	str = (gchar *)g_malloc(size);
	editor_msg(SCI_GETTEXT, size, (sptr_t)str);

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

	editor_msg(SCI_BEGINUNDOACTION);
	editor_msg(SCI_CLEARALL);
	editor_msg(SCI_APPENDTEXT, size, (sptr_t)contents);
	editor_msg(SCI_ENDUNDOACTION);

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
	editor_msg(SCI_CLEARALL);

	/* FIXME: prevent excessive allocations by reading file into buffer */
	if (!g_file_get_contents(filename, &contents, &size, NULL))
		return false;
	editor_msg(SCI_APPENDTEXT, size, (sptr_t)contents);
	g_free(contents);

	editor_msg(SCI_GOTOPOS, 0);
	editor_msg(SCI_SETSAVEPOINT);

	set_filename(filename);

	return true;
}

void
Buffer::close(void)
{
	LIST_REMOVE(this, buffers);

	if (filename)
		message_display(GTK_MESSAGE_INFO,
				"Removed file \"%s\" from the ring",
				filename);
	else
		message_display(GTK_MESSAGE_INFO,
				"Removed unnamed file from the ring.");
}

Buffer *
Ring::find(const gchar *filename)
{
	Buffer *cur;

	LIST_FOREACH(cur, &head, buffers)
		if (!g_strcmp0(cur->filename, filename))
			return cur;

	return NULL;
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

			message_display(GTK_MESSAGE_INFO,
					"Added file \"%s\" to ring", filename);
		} else {
			buffer->edit();
			buffer->set_filename(filename);

			if (filename)
				message_display(GTK_MESSAGE_INFO,
						"Added new file \"%s\" to ring",
						filename);
			else
				message_display(GTK_MESSAGE_INFO,
						"Added new unnamed file to ring.");
		}
	}

	qregisters.current = NULL;
	current = buffer;

	return new_in_ring;
}

void
Ring::close(void)
{
	Buffer *buffer = current;

	buffer->close();
	current = buffer->next() ? : first();
	if (!current)
		edit(NULL);

	delete buffer;
}

Ring::~Ring()
{
	Buffer *buffer, *next;

	LIST_FOREACH_SAFE(buffer, &head, buffers, next)
		delete buffer;
}

/*
 * Command states
 */

void
StateFile::do_edit(const gchar *filename)
{
	if (ring.current)
		ring.undo_edit();
	else /* qregisters.current != NULL */
		qregisters.undo_edit();
	if (ring.edit(filename))
		ring.undo_close();
}

void
StateFile::initial(void)
{
	gint64 id = expressions.pop_num_calc(1, -1);

	if (id == 0) {
		for (Buffer *cur = ring.first(); cur; cur = cur->next())
			gtk_info_popup_add_filename(filename_popup,
						    GTK_INFO_POPUP_FILE,
						    cur->filename ? : "(Unnamed)",
						    cur == ring.current);

		gtk_widget_show(GTK_WIDGET(filename_popup));
	}
}

State *
StateFile::done(const gchar *str)
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
		from = editor_msg(SCI_GETCURRENTPOS);
		sptr_t line = editor_msg(SCI_LINEFROMPOSITION, from) +
			      expressions.pop_num_calc();
		len = editor_msg(SCI_POSITIONFROMLINE, line) - from;

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
	editor_msg(SCI_GETTEXTRANGE, 0, (sptr_t)&tr);

	reg->undo_set_string();
	reg->set_string(tr.lpstrText);
	g_free(tr.lpstrText);

	return &States::start;
}
