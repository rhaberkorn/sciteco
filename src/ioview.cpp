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
 * A wrapper around g_io_channel_read_chars() that also
 * performs automatic EOL translation (if enabled) in a
 * more or less efficient manner.
 * Unlike g_io_channel_read_chars(), this returns an
 * offset and length into the buffer with normalized
 * EOL character.
 * The function must therefore be called iteratively on
 * on the same buffer while it returns G_IO_STATUS_NORMAL.
 *
 * @param channel The GIOChannel to read from.
 ' @param buffer Used to store blocks.
 * @param buffer_len Size of buffer.
 * @param read_len Total number of bytes read into buffer.
 *                 Must be provided over the lifetime of buffer
 *                 and initialized with 0.
 * @param offset If a block could be read (G_IO_STATUS_NORMAL),
 *               this will be set to indicate its beginning in
 *               buffer. Should be initialized to 0.
 * @param block_len Will be set to the block length.
 *                  Should be initialized to 0.
 * @param state Opaque state that must persist for the lifetime
 *              of the channel. Must be initialized with 0.
 * @param eol_style Will be set to the EOL style guessed from
 *                  the data in channel (if the data allows it).
 *                  Should be initialized with -1 (unknown).
 * @param eol_style_consistent Will be set to TRUE if
 *                             inconsistent EOL styles are detected.
 * @param error If not NULL and an error occurred, it is set to
 *              the error. It should be initialized to -1.
 * @return A GIOStatus as returned by g_io_channel_read_chars()
 */
GIOStatus
IOView::channel_read_with_eol(GIOChannel *channel,
                              gchar *buffer, gsize buffer_len,
                              gsize &read_len,
                              guint &offset, gsize &block_len,
                              gint &state, gint &eol_style,
                              gboolean &eol_style_inconsistent,
                              GError **error)
{
	GIOStatus status;

	if (state < 0) {
		/* a CRLF was last translated */
		block_len++;
		state = '\n';
	}
	offset += block_len;

	if (offset == read_len) {
		offset = 0;

		status = g_io_channel_read_chars(channel, buffer, buffer_len,
		                                 &read_len, error);
		if (status == G_IO_STATUS_EOF && state == '\r') {
			/*
			 * Very last character read is CR.
			 * If this is the only EOL so far, the
			 * EOL style is MAC.
			 * This is also executed if auto-eol is disabled
			 * but it doesn't hurt.
			 */
			if (eol_style < 0)
				eol_style = SC_EOL_CR;
			else if (eol_style != SC_EOL_CR)
				eol_style_inconsistent = TRUE;
		}
		if (status != G_IO_STATUS_NORMAL)
			return status;

		if (!(Flags::ed & Flags::ED_AUTOEOL)) {
			/*
			 * No EOL translation - always return entire
			 * buffer
			 */
			block_len = read_len;
			return G_IO_STATUS_NORMAL;
		}
	}

	/*
	 * Return data with automatic EOL translation.
	 * Every EOL sequence is normalized to LF and
	 * the first sequence determines the documents
	 * EOL style.
	 * This loop is executed for every byte of the
	 * file/stream, so it was important to optimize
	 * it. Specifically, the number of returns
	 * is minimized by keeping a pointer to
	 * the beginning of a block of data in the buffer
	 * which already has LFs (offset).
	 * Mac EOLs can be converted to UNIX EOLs directly
	 * in the buffer.
	 * So if their EOLs are consistent, the function
	 * will return one block for the entire buffer.
	 * When reading a file with DOS EOLs, there will
	 * be one call per line which is significantly slower.
	 */
	for (guint i = offset; i < read_len; i++) {
		switch (buffer[i]) {
		case '\n':
			if (state == '\r') {
				if (eol_style < 0)
					eol_style = SC_EOL_CRLF;
				else if (eol_style != SC_EOL_CRLF)
					eol_style_inconsistent = TRUE;

				/*
				 * Return block. CR has already
				 * been made LF in `buffer`.
				 */
				block_len = i-offset;
				/* next call will skip the CR */
				state = -1;
				return G_IO_STATUS_NORMAL;
			}

			if (eol_style < 0)
				eol_style = SC_EOL_LF;
			else if (eol_style != SC_EOL_LF)
				eol_style_inconsistent = TRUE;
			/*
			 * No conversion necessary and no need to
			 * return block yet.
			 */
			state = '\n';
			break;

		case '\r':
			if (state == '\r') {
				if (eol_style < 0)
					eol_style = SC_EOL_CR;
				else if (eol_style != SC_EOL_CR)
					eol_style_inconsistent = TRUE;
			}

			/*
			 * Convert CR to LF in `buffer`.
			 * This way more than one line using
			 * Mac EOLs can be returned at once.
			 */
			buffer[i] = '\n';
			state = '\r';
			break;

		default:
			if (state == '\r') {
				if (eol_style < 0)
					eol_style = SC_EOL_CR;
				else if (eol_style != SC_EOL_CR)
					eol_style_inconsistent = TRUE;
			}
			state = buffer[i];
			break;
		}
	}

	/*
	 * Return remaining block.
	 * With UNIX/MAC EOLs, this will usually be the
	 * entire `buffer`
	 */
	block_len = read_len-offset;
	return G_IO_STATUS_NORMAL;
}

/**
 * Loads the view's document by reading all data from
 * a GIOChannel.
 * The EOL style is guessed from the channel's data
 * (if AUTOEOL is enabled).
 * This assumes that the channel is blocking.
 * Also it tries to guess the size of the file behind
 * channel in order to preallocate memory in Scintilla.
 *
 * @param channel Channel to read from.
 * @param error Glib error or NULL.
 * @returns A GIOStatus as returned by g_io_channel_read_chars()
 */
GIOStatus
IOView::load(GIOChannel *channel, GError **error)
{
	GIOStatus status;
	GStatBuf stat_buf;

	gchar buffer[1024];
	gsize read_len = 0;
	guint offset = 0;
	gsize block_len = 0;
	gint state = 0;		/* opaque state */
	gint eol_style = -1;	/* yet unknown */
	gboolean eol_style_inconsistent = FALSE;

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

	for (;;) {
		status = channel_read_with_eol(
			channel, buffer, sizeof(buffer),
		        read_len, offset, block_len, state,
		        eol_style, eol_style_inconsistent,
		        error
		);
		if (status != G_IO_STATUS_NORMAL)
			break;

		ssm(SCI_APPENDTEXT, block_len, (sptr_t)(buffer+offset));
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
	if (eol_style >= 0)
		ssm(SCI_SETEOLMODE, eol_style);

	if (eol_style_inconsistent)
		interface.msg(InterfaceCurrent::MSG_WARNING,
		              "Inconsistent EOL styles normalized");

	ssm(SCI_ENDUNDOACTION);
	return status;
}

/**
 * Load view's document from file.
 */
void
IOView::load(const gchar *filename)
{
	GError *error = NULL;
	GIOChannel *channel;
	GIOStatus status;

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

	status = load(channel, &error);
	/* also closes file: */
	g_io_channel_unref(channel);
	if (status == G_IO_STATUS_ERROR) {
		Error err("Error reading file \"%s\": %s",
		          filename, error->message);
		g_error_free(error);
		throw err;
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

	gsize
	get_size(void) const
	{
		gsize ret = sizeof(*this) + strlen(filename) + 1;

		if (savepoint)
			ret += strlen(savepoint) + 1;

		return ret;
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

GIOStatus
IOView::save(GIOChannel *channel, guint position, gsize len,
             gsize *bytes_written, gint &state, GError **error)
{
	const gchar *buffer;
	const gchar *eol_seq;
	gchar last_c;
	guint i = 0;
	guint block_start;
	gsize block_written;

	GIOStatus status;

	enum {
		SAVE_STATE_START = 0,
		SAVE_STATE_WRITE_LF
	};

	buffer = (const gchar *)ssm(SCI_GETRANGEPOINTER,
	                            position, (sptr_t)len);

	if (!(Flags::ed & Flags::ED_AUTOEOL))
		/*
		 * Write without EOL-translation:
		 * `state` is not required
		 */
		return g_io_channel_write_chars(channel, buffer, len,
		                                bytes_written, error);

	/*
	 * Write to stream with EOL-translation.
	 * The document's EOL mode tells us what was guessed
	 * when its content was read in (presumably from a file)
	 * but might have been changed manually by the user.
	 * NOTE: This code assumes that the output stream is
	 * buffered, since otherwise it would be slower
	 * (has been benchmarked).
	 * NOTE: The loop is executed for every character
	 * in `buffer` and has been optimized for minimal
	 * function (i.e. GIOChannel) calls.
	 */
	*bytes_written = 0;
	if (state == SAVE_STATE_WRITE_LF) {
		/* complete writing a CRLF sequence */
		status = g_io_channel_write_chars(channel, "\n", 1, NULL, error);
		if (status != G_IO_STATUS_NORMAL)
			return status;
		state = SAVE_STATE_START;
		(*bytes_written)++;
		i++;
	}

	eol_seq = get_eol_seq(ssm(SCI_GETEOLMODE));
	last_c = ssm(SCI_GETCHARAT, position-1);

	block_start = i;
	while (i < len) {
		switch (buffer[i]) {
		case '\n':
			if (last_c == '\r') {
				/* EOL sequence already written */
				(*bytes_written)++;
				block_start = i+1;
				break;
			}
			/* fall through */
		case '\r':
			status = g_io_channel_write_chars(channel, buffer+block_start,
			                                  i-block_start, &block_written, error);
			*bytes_written += block_written;
			if (status != G_IO_STATUS_NORMAL ||
			    block_written < i-block_start)
				return status;

			status = g_io_channel_write_chars(channel, eol_seq,
			                                  -1, &block_written, error);
			if (status != G_IO_STATUS_NORMAL)
				return status;
			if (eol_seq[block_written]) {
				/* incomplete EOL seq - we have written CR of CRLF */
				state = SAVE_STATE_WRITE_LF;
				return G_IO_STATUS_NORMAL;
			}
			(*bytes_written)++;

			block_start = i+1;
			break;
		}

		last_c = buffer[i++];
	}

	/*
	 * Write out remaining block (i.e. line)
	 */
	status = g_io_channel_write_chars(channel, buffer+block_start,
	                                  len-block_start, &block_written, error);
	*bytes_written += block_written;
	return status;
}

gboolean
IOView::save(GIOChannel *channel, GError **error)
{
	sptr_t gap;
	gsize size;
	gsize bytes_written;
	gint state = 0;

	/* write part of buffer before gap */
	gap = ssm(SCI_GETGAPPOSITION);
	if (gap > 0) {
		if (save(channel, 0, gap, &bytes_written, state, error) == G_IO_STATUS_ERROR)
			return FALSE;
		g_assert(bytes_written == (gsize)gap);
	}

	/* write part of buffer after gap */
	size = ssm(SCI_GETLENGTH) - gap;
	if (size > 0) {
		if (save(channel, gap, size, &bytes_written, state, error) == G_IO_STATUS_ERROR)
			return FALSE;
		g_assert(bytes_written == size);
	}

	return TRUE;
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

	if (!save(channel, &error)) {
		Error err("Error writing file \"%s\": %s", filename, error->message);
		g_error_free(error);
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
