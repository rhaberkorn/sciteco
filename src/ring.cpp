/*
 * Copyright (C) 2012-2014 Robin Haberkorn
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
#include <unistd.h>
#include <stdio.h>
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
#undef interface
#endif

namespace SciTECO {

namespace States {
	StateEditFile	editfile;
	StateSaveFile	savefile;
}

#ifdef G_OS_WIN32

typedef DWORD FileAttributes;
/* INVALID_FILE_ATTRIBUTES already defined */

static inline FileAttributes
get_file_attributes(const gchar *filename)
{
	return GetFileAttributes((LPCTSTR)filename);
}

static inline void
set_file_attributes(const gchar *filename, FileAttributes attrs)
{
	SetFileAttributes((LPCTSTR)filename, attrs);
}

#else

typedef int FileAttributes;
#define INVALID_FILE_ATTRIBUTES (-1)

static inline FileAttributes
get_file_attributes(const gchar *filename)
{
	struct stat buf;

	return g_stat(filename, &buf) ? INVALID_FILE_ATTRIBUTES : buf.st_mode;
}

static inline void
set_file_attributes(const gchar *filename, FileAttributes attrs)
{
	g_chmod(filename, attrs);
}

#endif /* !G_OS_WIN32 */

void
Buffer::UndoTokenClose::run(void)
{
	ring.close(buffer);
	/* NOTE: the buffer is NOT deleted on Token destruction */
	delete buffer;
}

/*
 * The following simple implementation of file reading is actually the
 * most efficient and useful in the common case of editing small files,
 * since
 * a) it works with minimal number of syscalls and
 * b) small files cause little temporary memory overhead.
 * Reading large files however could be very inefficient since the file
 * must first be read into memory and then copied in-memory. Also it could
 * result in thrashing.
 * Alternatively we could iteratively read into a smaller buffer trading
 * in speed against (temporary) memory consumption.
 * The best way to do it could be memory mapping the file as we could
 * let Scintilla copy from the file's virtual memory directly.
 * Unfortunately since every page of the mapped file is
 * only touched once by Scintilla TLB caching is useless and the TLB is
 * effectively thrashed with entries of the mapped file.
 * This results in the doubling of page faults and weighs out the other
 * advantages of memory mapping (has been benchmarked).
 *
 * So in the future, the following approach could be implemented:
 * 1.) On Unix/Posix, mmap() one page at a time, hopefully preventing
 *     TLB thrashing.
 * 2.) On other platforms read into and copy from a statically sized buffer
 *     (perhaps page-sized)
 */
void
Buffer::load(const gchar *filename)
{
	gchar *contents;
	gsize size;

	GError *gerror = NULL;

	if (!g_file_get_contents(filename, &contents, &size, &gerror))
		throw State::GError(gerror);

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
Ring::find(tecoInt id)
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
Ring::edit(tecoInt id)
{
	Buffer *buffer = find(id);

	if (!buffer)
		return false;

	current_doc_update();

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

	current_doc_update();

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
	FileAttributes orig_attrs;
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
			if (orig_attrs != INVALID_FILE_ATTRIBUTES)
				set_file_attributes(buffer->filename,
						    orig_attrs);
#endif
		} else {
			interface.msg(Interface::MSG_WARNING,
				      "Unable to restore save point file \"%s\"",
				      savepoint);
		}
	}
};

static inline FileAttributes
make_savepoint(Buffer *buffer)
{
	gchar *dirname, *basename, *savepoint;
	gchar savepoint_basename[FILENAME_MAX];

	FileAttributes attributes = get_file_attributes(buffer->filename);

	basename = g_path_get_basename(buffer->filename);
	g_snprintf(savepoint_basename, sizeof(savepoint_basename),
		   ".teco-%s-%d", basename, buffer->savepoint_id);
	g_free(basename);
	dirname = g_path_get_dirname(buffer->filename);
	savepoint = g_build_filename(dirname, savepoint_basename, NIL);
	g_free(dirname);

	if (!g_rename(buffer->filename, savepoint)) {
		UndoTokenRestoreSavePoint *token;

		buffer->savepoint_id++;
		token = new UndoTokenRestoreSavePoint(savepoint, buffer);
#ifdef G_OS_WIN32
		token->orig_attrs = attributes;
		if (attributes != INVALID_FILE_ATTRIBUTES)
			set_file_attributes(savepoint,
					    attributes | FILE_ATTRIBUTE_HIDDEN);
#endif
		undo.push(token);
	} else {
		interface.msg(Interface::MSG_WARNING,
			      "Unable to create save point file \"%s\"",
			      savepoint);
		g_free(savepoint);
	}

	return attributes;
}

#endif /* !G_OS_UNIX */

bool
Ring::save(const gchar *filename)
{
	const void *buffer;
	sptr_t gap;
	size_t size;
	FILE *file;

#ifdef G_OS_UNIX
	struct stat file_stat;
	file_stat.st_uid = -1;
	file_stat.st_gid = -1;
#endif
	FileAttributes attributes = INVALID_FILE_ATTRIBUTES;

	if (!current)
		return false;

	if (!filename && !current->filename)
		return false;

	/*
	 * Undirtify
	 * NOTE: info update is performed by current->set_filename()
	 */
	interface.undo_info_update(current);
	undo.push_var(current->dirty) = false;

	/*
	 * FIXME: necessary also if the filename was not specified but the file
	 * is (was) new, in order to canonicalize the filename.
	 * May be circumvented by cananonicalizing without requiring the file
	 * name to exist (like readlink -f)
	 * NOTE: undo_info_update is already called above
	 */
	undo.push_str(current->filename);
	current->set_filename(filename ? : current->filename);

	if (undo.enabled) {
		if (g_file_test(current->filename, G_FILE_TEST_IS_REGULAR)) {
#ifdef G_OS_UNIX
			g_stat(current->filename, &file_stat);
#endif
			attributes = make_savepoint(current);
		} else {
			undo.push(new UndoTokenRemoveFile(current->filename));
		}
	}

	/* leaves mode intact if file exists */
	file = g_fopen(current->filename, "w");
	if (!file)
		return false;

	/* write part of buffer before gap */
	gap = interface.ssm(SCI_GETGAPPOSITION);
	if (gap > 0) {
		buffer = (const void *)interface.ssm(SCI_GETRANGEPOINTER,
						     0, gap);
		if (!fwrite(buffer, (size_t)gap, 1, file)) {
			fclose(file);
			return false;
		}
	}

	/* write part of buffer after gap */
	size = interface.ssm(SCI_GETLENGTH) - gap;
	if (size > 0) {
		buffer = (const void *)interface.ssm(SCI_GETRANGEPOINTER,
						     gap, size);
		if (!fwrite(buffer, size, 1, file)) {
			fclose(file);
			return false;
		}
	}

	/* if file existed but has been renamed, restore attributes */
	if (attributes != INVALID_FILE_ATTRIBUTES)
		set_file_attributes(current->filename, attributes);
#ifdef G_OS_UNIX
	/*
	 * only a good try to inherit owner since process user must have
	 * CHOWN capability traditionally reserved to root only.
	 * That's why we don't handle the return value and are spammed
	 * with unused-result warnings by GCC. There is NO sane way to avoid
	 * this warning except, adding -Wno-unused-result which disabled all
	 * such warnings.
	 */
	fchown(fileno(file), file_stat.st_uid, file_stat.st_gid);
#endif

	fclose(file);

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

	buffer->update();
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
			resolved = g_build_filename(cwd, path, NIL);
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
StateEditFile::do_edit(const gchar *filename)
{
	if (ring.current)
		ring.undo_edit();
	else /* QRegisters::current != NULL */
		QRegisters::undo_edit();
	ring.edit(filename);
}

void
StateEditFile::do_edit(tecoInt id)
{
	if (ring.current)
		ring.undo_edit();
	else /* QRegisters::current != NULL */
		QRegisters::undo_edit();
	if (!ring.edit(id))
		throw Error("Invalid buffer id %" TECO_INTEGER_FORMAT, id);
}

/*$
 * [n]EB[file]$ -- Open or edit file
 * nEB$
 *
 * Opens or edits the file with name <file>.
 * If <file> is not in the buffer ring it is opened,
 * added to the ring and set as the currently edited
 * buffer.
 * If it already exists in the ring, it is merely
 * made the current file.
 * <file> may be omitted in which case the default
 * unnamed buffer is created/edited.
 * If an argument is specified as 0, EB will additionally
 * display the buffer ring contents in the window's popup
 * area.
 * Naturally this only has any effect in interactive
 * mode.
 *
 * <file> may also be a glob-pattern, in which case
 * all files matching the pattern are opened/edited.
 *
 * File names of buffers in the ring are normalized
 * by making them absolute.
 * Any comparison on file names is performed using
 * guessed or actual absolute file paths, so that
 * one file may be referred to in many different ways
 * (paths).
 *
 * <file> does not have to exist on disk.
 * In this case, an empty buffer is created and its
 * name is guessed from <file>.
 * When the newly created buffer is first saved,
 * the file is created on disk and the buffer's name
 * will be updated to the absolute path of the file
 * on disk.
 *
 * File names may also be tab-completed and string building
 * characters are enabled by default.
 *
 * If <n> is greater than zero, the string argument
 * must be empty.
 * Instead <n> selects a buffer from the ring to edit.
 * A value of 1 denotes the first buffer, 2 the second,
 * ecetera.
 */
void
StateEditFile::initial(void)
{
	tecoInt id = expressions.pop_num_calc(1, -1);

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
StateEditFile::done(const gchar *str)
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
								    NIL);
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

/*$
 * EW$ -- Save or rename current buffer
 * EWfile$
 *
 * Saves the current buffer to disk.
 * If the buffer was dirty, it will be clean afterwards.
 * If the string argument <file> is not empty,
 * the buffer is saved with the specified file name
 * and is renamed in the ring.
 *
 * In interactive mode, EW is executed immediately and
 * may be rubbed out.
 * In order to support that, \*(ST creates so called
 * save point files.
 * It does not merely overwrite existing files when saving
 * but moves them to save point files instead.
 * Save point files are called \(lq.teco-<filename>-<n>\(rq
 * where <filename> is the name of the saved file and <n> is
 * a number that is increased with every save operation.
 * Save point files are always created in the same directory
 * as the original file to ensure that no copying of the file
 * on disk is necessary but only a rename of the file.
 * When rubbing out the EW command, \*(ST restores the latest
 * save point file by moving (renaming) it back to its
 * original path - also not requiring any on-disk copying.
 * \*(ST is impossible to crash, but just in case it still
 * does it may leave behind these save point files which
 * must be manually deleted by the user.
 * Otherwise save point files are deleted on command line
 * termination.
 *
 * File names may also be tab-completed and string building
 * characters are enabled by default.
 */
State *
StateSaveFile::done(const gchar *str)
{
	BEGIN_EXEC(&States::start);

	if (!ring.save(*str ? str : NULL))
		throw Error("Unable to save file");

	return &States::start;
}

} /* namespace SciTECO */
