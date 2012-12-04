/*
 * Copyright (C) 2012 Robin Haberkorn
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <bsd/sys/queue.h>

#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>

#include <Scintilla.h>

#include "sciteco.h"
#include "interface.h"
#include "undo.h"
#include "parser.h"
#include "expressions.h"
#include "ring.h"

#ifdef HAVE_WINDOWS_H
/* here it shouldn't cause conflicts with other headers */
#include <windows.h>

/* still need to clean up */
#ifdef interface
#undef interface
#endif
#endif

namespace States {
	StateEditFile	editfile;
	StateSaveFile	savefile;
}

Ring ring;

void
Buffer::UndoTokenClose::run(void)
{
	ring.close(buffer);
	/* NOTE: the buffer is NOT deleted on Token destruction */
	delete buffer;
}

bool
Buffer::load(const gchar *filename)
{
	gchar *contents;
	gsize size;

	/* FIXME: prevent excessive allocations by reading file into buffer */
	if (!g_file_get_contents(filename, &contents, &size, NULL))
		return false;

	edit();

	interface.ssm(SCI_BEGINUNDOACTION);
	interface.ssm(SCI_CLEARALL);
	interface.ssm(SCI_APPENDTEXT, size, (sptr_t)contents);
	interface.ssm(SCI_ENDUNDOACTION);

	g_free(contents);

	/* NOTE: currently buffer cannot be dirty */
#if 0
	interface.undo_info_update(this);
	undo.push_var(dirty);
	dirty = false;
#endif

	set_filename(filename);

	return true;
}

void
Ring::UndoTokenEdit::run(void)
{
	/*
	 * assumes that buffer still has correct prev/next
	 * pointers
	 */
	if (buffer->next())
		TAILQ_INSERT_BEFORE(buffer->next(), buffer, buffers);
	else
		TAILQ_INSERT_TAIL(&ring->head, buffer, buffers);

	ring->current = buffer;
	buffer->edit();
	buffer = NULL;
}

Buffer *
Ring::find(const gchar *filename)
{
	gchar *resolved = get_absolute_path(filename);
	Buffer *cur;

	TAILQ_FOREACH(cur, &head, buffers)
		if (!g_strcmp0(cur->filename, resolved))
			break;

	g_free(resolved);
	return cur;
}

Buffer *
Ring::find(gint64 id)
{
	Buffer *cur;

	TAILQ_FOREACH(cur, &head, buffers)
		if (!--id)
			break;

	return cur;
}

void
Ring::dirtify(void)
{
	if (!current || current->dirty)
		return;

	interface.undo_info_update(current);
	undo.push_var(current->dirty);
	current->dirty = true;
	interface.info_update(current);
}

bool
Ring::is_any_dirty(void)
{
	Buffer *cur;

	TAILQ_FOREACH(cur, &head, buffers)
		if (cur->dirty)
			return true;

	return false;
}

bool
Ring::edit(gint64 id)
{
	Buffer *buffer = find(id);

	if (!buffer)
		return false;

	current_save_dot();

	QRegisters::current = NULL;
	current = buffer;
	buffer->edit();

	QRegisters::hook(QRegisters::HOOK_EDIT);

	return true;
}

void
Ring::edit(const gchar *filename)
{
	Buffer *buffer = find(filename);

	current_save_dot();

	QRegisters::current = NULL;
	if (buffer) {
		current = buffer;
		buffer->edit();

		QRegisters::hook(QRegisters::HOOK_EDIT);
	} else {
		buffer = new Buffer();
		TAILQ_INSERT_TAIL(&head, buffer, buffers);

		current = buffer;
		undo_close();

		if (filename && g_file_test(filename, G_FILE_TEST_IS_REGULAR)) {
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

		QRegisters::hook(QRegisters::HOOK_ADD);
	}
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
					  token->attributes |
					  FILE_ATTRIBUTE_HIDDEN);
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

	/* FIXME: improve by writing before and after document gap */
	buffer = (const gchar *)interface.ssm(SCI_GETCHARACTERPOINTER);
	size = interface.ssm(SCI_GETLENGTH);

	if (!g_file_set_contents(filename, buffer, size, NULL))
		return false;

	interface.undo_info_update(current);
	undo.push_var(current->dirty);
	current->dirty = false;

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
Ring::close(Buffer *buffer)
{
	TAILQ_REMOVE(&head, buffer, buffers);

	if (buffer->filename)
		interface.msg(Interface::MSG_INFO,
			      "Removed file \"%s\" from the ring",
			      buffer->filename);
	else
		interface.msg(Interface::MSG_INFO,
			      "Removed unnamed file from the ring.");
}

void
Ring::close(void)
{
	Buffer *buffer = current;

	buffer->dot = interface.ssm(SCI_GETCURRENTPOS);
	close(buffer);
	current = buffer->next() ? : buffer->prev();
	/* transfer responsibility to UndoToken object */
	undo.push(new UndoTokenEdit(this, buffer));

	if (current) {
		current->edit();
		QRegisters::hook(QRegisters::HOOK_EDIT);
	} else {
		edit((const gchar *)NULL);
	}
}

Ring::~Ring()
{
	Buffer *buffer, *next;

	TAILQ_FOREACH_SAFE(buffer, &head, buffers, next)
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

#elif defined(G_OS_WIN32)

gchar *
get_absolute_path(const gchar *path)
{
	TCHAR buf[MAX_PATH];
	gchar *resolved = NULL;

	if (path && GetFullPathName(path, sizeof(buf), buf, NULL))
		resolved = g_strdup(buf);

	return resolved;
}

#else

/*
 * FIXME: I doubt that works on any platform...
 */
gchar *
get_absolute_path(const gchar *path)
{
	return path ? g_file_read_link(path, NULL) : NULL;
}

#endif /* !G_OS_UNIX && !G_OS_WIN32 */

/*
 * Command states
 */

void
StateEditFile::do_edit(const gchar *filename) throw (Error)
{
	if (ring.current)
		ring.undo_edit();
	else /* QRegisters::current != NULL */
		QRegisters::undo_edit();
	ring.edit(filename);
}

void
StateEditFile::do_edit(gint64 id) throw (Error)
{
	if (ring.current)
		ring.undo_edit();
	else /* QRegisters::current != NULL */
		QRegisters::undo_edit();
	if (!ring.edit(id))
		throw Error("Invalid buffer id %" G_GINT64_FORMAT, id);
}

void
StateEditFile::initial(void) throw (Error)
{
	gint64 id = expressions.pop_num_calc(1, -1);

	allowFilename = true;

	if (id == 0) {
		for (Buffer *cur = ring.first(); cur; cur = cur->next())
			interface.popup_add(Interface::POPUP_FILE,
					    cur->filename ? : "(Unnamed)",
					    cur == ring.current);

		interface.popup_show();
	} else if (id > 0) {
		allowFilename = false;
		do_edit(id);
	}
}

State *
StateEditFile::done(const gchar *str) throw (Error)
{
	BEGIN_EXEC(&States::start);

	if (!allowFilename) {
		if (*str)
			throw Error("If a buffer is selected by id, the <EB> "
				    "string argument must be empty");

		return &States::start;
	}

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
StateSaveFile::done(const gchar *str) throw (Error)
{
	BEGIN_EXEC(&States::start);

	if (!ring.save(*str ? str : NULL))
		throw Error("Unable to save file");

	return &States::start;
}
