/*
 * Copyright (C) 2012-2016 Robin Haberkorn
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
#include <errno.h>
#include <sys/stat.h>

#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>

#include <Scintilla.h>

#include "sciteco.h"
#include "interface.h"
#include "undo.h"
#include "error.h"
#include "qregisters.h"
#include "eol.h"
#include "ioview.h"

#ifdef HAVE_WINDOWS_H
/* here it shouldn't cause conflicts with other headers */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace SciTECO {

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

/**
 * Loads the view's document by reading all data from
 * a GIOChannel.
 * The EOL style is guessed from the channel's data
 * (if AUTOEOL is enabled).
 * This assumes that the channel is blocking.
 * Also it tries to guess the size of the file behind
 * channel in order to preallocate memory in Scintilla.
 *
 * Any error reading the GIOChannel is propagated as
 * an exception.
 *
 * @param channel Channel to read from.
 */
void
IOView::load(GIOChannel *channel)
{
	GStatBuf stat_buf;

	EOLReaderGIO reader(channel);

	ssm(SCI_BEGINUNDOACTION);
	ssm(SCI_CLEARALL);

	/*
	 * Preallocate memory based on the file size.
	 * May waste a few bytes if file contains DOS EOLs
	 * and EOL translation is enabled, but is faster.
	 * NOTE: g_io_channel_unix_get_fd() should report the correct fd
	 * on Windows, too.
	 */
	stat_buf.st_size = 0;
	if (!fstat(g_io_channel_unix_get_fd(channel), &stat_buf) &&
	    stat_buf.st_size > 0)
		ssm(SCI_ALLOCATE, stat_buf.st_size);

	try {
		const gchar *data;
		gsize data_len;

		while ((data = reader.convert(data_len)))
			ssm(SCI_APPENDTEXT, data_len, (sptr_t)data);
	} catch (...) {
		ssm(SCI_ENDUNDOACTION);
		throw; /* forward */
	}

	/*
	 * EOL-style guessed.
	 * Save it as the buffer's EOL mode, so save()
	 * can restore the original EOL-style.
	 * If auto-EOL-translation is disabled, this cannot
	 * have been guessed and the buffer's EOL mode should
	 * have a platform default.
	 * If it is enabled but the stream does not contain any
	 * EOL characters, the platform default is still assumed.
	 */
	if (reader.eol_style >= 0)
		ssm(SCI_SETEOLMODE, reader.eol_style);

	if (reader.eol_style_inconsistent)
		interface.msg(InterfaceCurrent::MSG_WARNING,
		              "Inconsistent EOL styles normalized");

	ssm(SCI_ENDUNDOACTION);
}

/**
 * Load view's document from file.
 */
void
IOView::load(const gchar *filename)
{
	GError *error = NULL;
	GIOChannel *channel;

	channel = g_io_channel_new_file(filename, "r", &error);
	if (!channel) {
		Error err("Error opening file \"%s\" for reading: %s",
		          filename, error->message);
		g_error_free(error);
		throw err;
	}

	/*
	 * The file loading algorithm does not need buffered
	 * streams, so disabling buffering should increase
	 * performance (slightly).
	 */
	g_io_channel_set_encoding(channel, NULL, NULL);
	g_io_channel_set_buffered(channel, FALSE);

	try {
		load(channel);
	} catch (Error &e) {
		Error err("Error reading file \"%s\": %s",
		          filename, e.description);
		g_io_channel_unref(channel);
		throw err;
	}

	/* also closes file: */
	g_io_channel_unref(channel);
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

static gint savepoint_id = 0;

class UndoTokenRestoreSavePoint : public UndoToken {
	gchar	*savepoint;
	gchar	*filename;

#ifdef G_OS_WIN32
	FileAttributes orig_attrs;
#endif

public:
	UndoTokenRestoreSavePoint(gchar *_savepoint, const gchar *_filename)
				 : savepoint(_savepoint), filename(g_strdup(_filename))
	{
#ifdef G_OS_WIN32
		orig_attrs = get_file_attributes(filename);
		if (orig_attrs != INVALID_FILE_ATTRIBUTES)
			set_file_attributes(savepoint,
			                    orig_attrs | FILE_ATTRIBUTE_HIDDEN);
#endif
	}

	~UndoTokenRestoreSavePoint()
	{
		if (savepoint) {
			g_unlink(savepoint);
			g_free(savepoint);
		}
		g_free(filename);

		savepoint_id--;
	}

	void
	run(void)
	{
		if (!g_rename(savepoint, filename)) {
			g_free(savepoint);
			savepoint = NULL;
#ifdef G_OS_WIN32
			if (orig_attrs != INVALID_FILE_ATTRIBUTES)
				set_file_attributes(filename,
						    orig_attrs);
#endif
		} else {
			interface.msg(InterfaceCurrent::MSG_WARNING,
				      "Unable to restore save point file \"%s\"",
				      savepoint);
		}
	}
};

static void
make_savepoint(const gchar *filename)
{
	gchar *dirname, *basename, *savepoint;
	gchar savepoint_basename[FILENAME_MAX];

	basename = g_path_get_basename(filename);
	g_snprintf(savepoint_basename, sizeof(savepoint_basename),
		   ".teco-%d-%s~", savepoint_id, basename);
	g_free(basename);
	dirname = g_path_get_dirname(filename);
	savepoint = g_build_filename(dirname, savepoint_basename, NIL);
	g_free(dirname);

	if (g_rename(filename, savepoint)) {
		interface.msg(InterfaceCurrent::MSG_WARNING,
			      "Unable to create save point file \"%s\"",
			      savepoint);
		g_free(savepoint);
		return;
	}
	savepoint_id++;

	/*
	 * NOTE: passes ownership of savepoint string to undo token.
	 */
	undo.push_own<UndoTokenRestoreSavePoint>(savepoint, filename);
}

#endif

void
IOView::save(GIOChannel *channel)
{
	EOLWriterGIO writer(channel, ssm(SCI_GETEOLMODE));
	sptr_t gap;
	gsize size;
	const gchar *buffer;
	gsize bytes_written;

	/* write part of buffer before gap */
	gap = ssm(SCI_GETGAPPOSITION);
	if (gap > 0) {
		buffer = (const gchar *)ssm(SCI_GETRANGEPOINTER, 0, gap);
		bytes_written = writer.convert(buffer, gap);
		g_assert(bytes_written == (gsize)gap);
	}

	/* write part of buffer after gap */
	size = ssm(SCI_GETLENGTH) - gap;
	if (size > 0) {
		buffer = (const gchar *)ssm(SCI_GETRANGEPOINTER, gap, (sptr_t)size);
		bytes_written = writer.convert(buffer, size);
		g_assert(bytes_written == size);
	}
}

void
IOView::save(const gchar *filename)
{
	GError *error = NULL;
	GIOChannel *channel;

#if defined(G_OS_UNIX) || defined(G_OS_HAIKU)
	GStatBuf file_stat;
	file_stat.st_uid = -1;
	file_stat.st_gid = -1;
#endif
	FileAttributes attributes = INVALID_FILE_ATTRIBUTES;

	if (undo.enabled) {
		if (g_file_test(filename, G_FILE_TEST_IS_REGULAR)) {
#if defined(G_OS_UNIX) || defined(G_OS_HAIKU)
			g_stat(filename, &file_stat);
#endif
			attributes = get_file_attributes(filename);
			make_savepoint(filename);
		} else {
			undo.push<UndoTokenRemoveFile>(filename);
		}
	}

	/* leaves access mode intact if file still exists */
	channel = g_io_channel_new_file(filename, "w", &error);
	if (!channel)
		throw GlibError(error);

	/*
	 * save(GIOChannel *, const gchar *) expects a buffered
	 * and blocking channel
	 */
	g_io_channel_set_encoding(channel, NULL, NULL);
	g_io_channel_set_buffered(channel, TRUE);

	try {
		save(channel);
	} catch (Error &e) {
		Error err("Error writing file \"%s\": %s", filename, e.description);
		g_io_channel_unref(channel);
		throw err;
	}

	/* if file existed but has been renamed, restore attributes */
	if (attributes != INVALID_FILE_ATTRIBUTES)
		set_file_attributes(filename, attributes);
#if defined(G_OS_UNIX) || defined(G_OS_HAIKU)
	/*
	 * only a good try to inherit owner since process user must have
	 * CHOWN capability traditionally reserved to root only.
	 * FIXME: We should probably fall back to another save point
	 * strategy.
	 */
	if (fchown(g_io_channel_unix_get_fd(channel),
	           file_stat.st_uid, file_stat.st_gid))
		interface.msg(InterfaceCurrent::MSG_WARNING,
			      "Unable to preserve owner of \"%s\": %s",
			      filename, g_strerror(errno));
#endif

	/* also closes file */
	g_io_channel_unref(channel);
}

/*
 * Auxiliary functions
 */

/**
 * Perform tilde expansion on a file name or path.
 *
 * This supports only strings with a "~" prefix.
 * A user name after "~" is not supported.
 * The $HOME environment variable/register is used to retrieve
 * the current user's home directory.
 */
gchar *
expand_path(const gchar *path)
{
	gchar *home, *ret;

	if (!path)
		path = "";

	if (path[0] != '~' || (path[1] && !G_IS_DIR_SEPARATOR(path[1])))
		return g_strdup(path);

	/*
	 * $HOME should not have a trailing directory separator since
	 * it is canonicalized to an absolute path at startup,
	 * but this ensures that a proper path is constructed even if
	 * it does (e.g. $HOME is changed later on).
	 */
	home = QRegisters::globals["$HOME"]->get_string();
	ret = g_build_filename(home, path+1, NIL);
	g_free(home);

	return ret;
}

#if defined(G_OS_UNIX) || defined(G_OS_HAIKU)

gchar *
get_absolute_path(const gchar *path)
{
	gchar buf[PATH_MAX];
	gchar *resolved;

	if (!path)
		return NULL;

	if (realpath(path, buf)) {
		resolved = g_strdup(buf);
	} else if (g_path_is_absolute(path)) {
		resolved = g_strdup(path);
	} else {
		gchar *cwd = g_get_current_dir();
		resolved = g_build_filename(cwd, path, NIL);
		g_free(cwd);
	}

	return resolved;
}

bool
file_is_visible(const gchar *path)
{
	gchar *basename = g_path_get_basename(path);
	bool ret = *basename != '.';

	g_free(basename);
	return ret;
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

bool
file_is_visible(const gchar *path)
{
	return !(get_file_attributes(path) & FILE_ATTRIBUTE_HIDDEN);
}

#else /* !G_OS_UNIX && !G_OS_HAIKU && !G_OS_WIN32 */

/*
 * This will never canonicalize relative paths.
 * I.e. the absolute path will often contain
 * relative components, even if `path` exists.
 * The only exception would be a simple filename
 * not containing any "..".
 */
gchar *
get_absolute_path(const gchar *path)
{
	gchar *resolved;

	if (!path)
		return NULL;

	if (g_path_is_absolute(path)) {
		resolved = g_strdup(path);
	} else {
		gchar *cwd = g_get_current_dir();
		resolved = g_build_filename(cwd, path, NIL);
		g_free(cwd);
	}

	return resolved;
}

/*
 * There's no platform-independent way to determine if a file
 * is visible/hidden, so we just assume that all files are
 * visible.
 */
bool
file_is_visible(const gchar *path)
{
	return true;
}

#endif /* !G_OS_UNIX && !G_OS_HAIKU && !G_OS_WIN32 */

} /* namespace SciTECO */
