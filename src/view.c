/*
 * Copyright (C) 2012-2021 Robin Haberkorn
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

#ifdef HAVE_WINDOWS_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>

#include <Scintilla.h>

#include "sciteco.h"
#include "file-utils.h"
#include "string-utils.h"
#include "interface.h"
#include "undo.h"
#include "error.h"
#include "qreg.h"
#include "eol.h"
#include "view.h"

/** @memberof teco_view_t */
void
teco_view_setup(teco_view_t *ctx)
{
	/*
	 * Start with or without undo collection,
	 * depending on teco_undo_enabled.
	 */
	teco_view_ssm(ctx, SCI_SETUNDOCOLLECTION, teco_undo_enabled, 0);

	teco_view_ssm(ctx, SCI_SETFOCUS, TRUE, 0);

	/*
	 * Some Scintilla implementations show the horizontal
	 * scroll bar by default.
	 * Ensure it is never displayed by default.
	 */
	teco_view_ssm(ctx, SCI_SETHSCROLLBAR, FALSE, 0);

	/*
	 * Only margin 1 is given a width by default.
	 * To provide a minimalist default view, it is disabled.
	 */
	teco_view_ssm(ctx, SCI_SETMARGINWIDTHN, 1, 0);

	/*
	 * Set some basic styles in order to provide
	 * a consistent look across UIs if no profile
	 * is used. This makes writing UI-agnostic profiles
	 * and color schemes easier.
	 * FIXME: Some settings like fonts should probably
	 * be set per UI (i.e. Scinterm doesn't use it,
	 * GTK might try to use a system-wide default
	 * monospaced font).
	 */
	teco_view_ssm(ctx, SCI_SETCARETSTYLE, CARETSTYLE_BLOCK, 0);
	teco_view_ssm(ctx, SCI_SETCARETPERIOD, 0, 0);
	teco_view_ssm(ctx, SCI_SETCARETFORE, 0xFFFFFF, 0);

	teco_view_ssm(ctx, SCI_STYLESETFORE, STYLE_DEFAULT, 0xFFFFFF);
	teco_view_ssm(ctx, SCI_STYLESETBACK, STYLE_DEFAULT, 0x000000);
	teco_view_ssm(ctx, SCI_STYLESETFONT, STYLE_DEFAULT, (sptr_t)"Courier");
	teco_view_ssm(ctx, SCI_STYLECLEARALL, 0, 0);

	/*
	 * FIXME: The line number background is apparently not
	 * affected by SCI_STYLECLEARALL
	 */
	teco_view_ssm(ctx, SCI_STYLESETBACK, STYLE_LINENUMBER, 0x000000);

	/*
	 * Use white as the default background color
	 * for call tips. Necessary since this style is also
	 * used for popup windows and we need to provide a sane
	 * default if no color-scheme is applied (and --no-profile).
	 */
	teco_view_ssm(ctx, SCI_STYLESETFORE, STYLE_CALLTIP, 0x000000);
	teco_view_ssm(ctx, SCI_STYLESETBACK, STYLE_CALLTIP, 0xFFFFFF);
}

TECO_DEFINE_UNDO_CALL(teco_view_ssm, teco_view_t *, unsigned int, uptr_t, sptr_t);

/** @memberof teco_view_t */
void
teco_view_set_representations(teco_view_t *ctx)
{
	static const char *reps[] = {
		"^@", "^A", "^B", "^C", "^D", "^E", "^F", "^G",
		"^H", "TAB" /* ^I */, "LF" /* ^J */, "^K", "^L", "CR" /* ^M */, "^N", "^O",
		"^P", "^Q", "^R", "^S", "^T", "^U", "^V", "^W",
		"^X", "^Y", "^Z", "$" /* ^[ */, "^\\", "^]", "^^", "^_"
	};

	for (guint cc = 0; cc < G_N_ELEMENTS(reps); cc++) {
		gchar buf[] = {(gchar)cc, '\0'};
		teco_view_ssm(ctx, SCI_SETREPRESENTATION, (uptr_t)buf, (sptr_t)reps[cc]);
	}
}

TECO_DEFINE_UNDO_CALL(teco_view_set_representations, teco_view_t *);

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
 * @param ctx The view to load.
 * @param channel Channel to read from.
 * @param error A GError.
 * @return FALSE in case of a GError.
 *
 * @memberof teco_view_t
 */
gboolean
teco_view_load_from_channel(teco_view_t *ctx, GIOChannel *channel, GError **error)
{
	teco_view_ssm(ctx, SCI_BEGINUNDOACTION, 0, 0);
	teco_view_ssm(ctx, SCI_CLEARALL, 0, 0);

	/*
	 * Preallocate memory based on the file size.
	 * May waste a few bytes if file contains DOS EOLs
	 * and EOL translation is enabled, but is faster.
	 * NOTE: g_io_channel_unix_get_fd() should report the correct fd
	 * on Windows, too.
	 */
	struct stat stat_buf = {.st_size = 0};
	if (!fstat(g_io_channel_unix_get_fd(channel), &stat_buf) &&
	    stat_buf.st_size > 0)
		teco_view_ssm(ctx, SCI_ALLOCATE, stat_buf.st_size, 0);

	g_auto(teco_eol_reader_t) reader;
	teco_eol_reader_init_gio(&reader, channel);

	for (;;) {
		/*
		 * NOTE: We don't have to free this data since teco_eol_reader_gio_convert()
		 * will point it into its internal buffer.
		 */
		teco_string_t str;

		GIOStatus rc = teco_eol_reader_convert(&reader, &str.data, &str.len, error);
		if (rc == G_IO_STATUS_ERROR) {
			teco_view_ssm(ctx, SCI_ENDUNDOACTION, 0, 0);
			return FALSE;
		}
		if (rc == G_IO_STATUS_EOF)
			break;

		teco_view_ssm(ctx, SCI_APPENDTEXT, str.len, (sptr_t)str.data);
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
		teco_view_ssm(ctx, SCI_SETEOLMODE, reader.eol_style, 0);

	if (reader.eol_style_inconsistent)
		teco_interface_msg(TECO_MSG_WARNING,
		                   "Inconsistent EOL styles normalized");

	teco_view_ssm(ctx, SCI_ENDUNDOACTION, 0, 0);
	return TRUE;
}

/**
 * Load view's document from file.
 *
 * @memberof teco_view_t
 */
gboolean
teco_view_load_from_file(teco_view_t *ctx, const gchar *filename, GError **error)
{
	g_autoptr(GIOChannel) channel = g_io_channel_new_file(filename, "r", error);
	if (!channel)
		return FALSE;

	/*
	 * The file loading algorithm does not need buffered
	 * streams, so disabling buffering should increase
	 * performance (slightly).
	 */
	g_io_channel_set_encoding(channel, NULL, NULL);
	g_io_channel_set_buffered(channel, FALSE);

	if (!teco_view_load_from_channel(ctx, channel, error)) {
		g_prefix_error(error, "Error reading file \"%s\": ", filename);
		return FALSE;
	}

	return TRUE;
}

#if 0

/*
 * TODO: on UNIX it may be better to open() the current file, unlink() it
 * and keep the file descriptor in the undo token.
 * When the operation is undone, the file descriptor's contents are written to
 * the file (which should be efficient enough because it is written to the same
 * filesystem). This way we could avoid messing around with save point files.
 */

#else

static gint savepoint_id = 0;

typedef struct {
#ifdef G_OS_WIN32
	teco_file_attributes_t orig_attrs;
#endif

	gchar *savepoint;
	gchar filename[];
} teco_undo_restore_savepoint_t;

static void
teco_undo_restore_savepoint_action(teco_undo_restore_savepoint_t *ctx, gboolean run)
{
	if (!run) {
		g_unlink(ctx->savepoint);
	} else if (!g_rename(ctx->savepoint, ctx->filename)) {
#ifdef G_OS_WIN32
		if (ctx->orig_attrs != TECO_FILE_INVALID_ATTRIBUTES)
			teco_file_set_attributes(ctx->filename, ctx->orig_attrs);
#endif
	} else {
		teco_interface_msg(TECO_MSG_WARNING,
		                   "Unable to restore save point file \"%s\"",
		                   ctx->savepoint);
	}

	g_free(ctx->savepoint);
	savepoint_id--;
}

static void
teco_undo_restore_savepoint_push(gchar *savepoint, const gchar *filename)
{
	teco_undo_restore_savepoint_t *ctx;

	ctx = teco_undo_push_size((teco_undo_action_t)teco_undo_restore_savepoint_action,
	                          sizeof(*ctx) + strlen(filename) + 1);
	if (ctx) {
		ctx->savepoint = savepoint;
		strcpy(ctx->filename, filename);

#ifdef G_OS_WIN32
		ctx->orig_attrs = teco_file_get_attributes(filename);
		if (ctx->orig_attrs != TECO_FILE_INVALID_ATTRIBUTES)
			teco_file_set_attributes(savepoint,
			                         ctx->orig_attrs | FILE_ATTRIBUTE_HIDDEN);
#endif
	} else {
		g_unlink(savepoint);
		g_free(savepoint);
		savepoint_id--;
	}
}

static void
teco_make_savepoint(const gchar *filename)
{
	gchar savepoint_basename[FILENAME_MAX];

	g_autofree gchar *basename = g_path_get_basename(filename);
	g_snprintf(savepoint_basename, sizeof(savepoint_basename),
		   ".teco-%d-%s~", savepoint_id, basename);
	g_autofree gchar *dirname = g_path_get_dirname(filename);
	gchar *savepoint = g_build_filename(dirname, savepoint_basename, NULL);

	if (g_rename(filename, savepoint)) {
		teco_interface_msg(TECO_MSG_WARNING,
		                   "Unable to create save point file \"%s\"",
		                   savepoint);
		g_free(savepoint);
		return;
	}
	savepoint_id++;

	/*
	 * NOTE: passes ownership of savepoint string to undo token.
	 */
	teco_undo_restore_savepoint_push(savepoint, filename);
}

#endif

/*
 * NOTE: Does not simply undo__g_unlink() since `filename` needs to be
 * memory managed.
 */
static void
sciteo_undo_remove_file_action(gchar *filename, gboolean run)
{
	if (run)
		g_unlink(filename);
}

static inline void
teco_undo_remove_file_push(const gchar *filename)
{
	gchar *ctx = teco_undo_push_size((teco_undo_action_t)sciteo_undo_remove_file_action,
	                                 strlen(filename)+1);
	if (ctx)
		strcpy(ctx, filename);
}

gboolean
teco_view_save_to_channel(teco_view_t *ctx, GIOChannel *channel, GError **error)
{
	g_auto(teco_eol_writer_t) writer;
	teco_eol_writer_init_gio(&writer, teco_view_ssm(ctx, SCI_GETEOLMODE, 0, 0), channel);

	/* write part of buffer before gap */
	sptr_t gap = teco_view_ssm(ctx, SCI_GETGAPPOSITION, 0, 0);
	if (gap > 0) {
		const gchar *buffer = (const gchar *)teco_view_ssm(ctx, SCI_GETRANGEPOINTER, 0, gap);
		gssize bytes_written = teco_eol_writer_convert(&writer, buffer, gap, error);
		if (bytes_written < 0)
			return FALSE;
		g_assert(bytes_written == (gsize)gap);
	}

	/* write part of buffer after gap */
	gsize size = teco_view_ssm(ctx, SCI_GETLENGTH, 0, 0) - gap;
	if (size > 0) {
		const gchar *buffer = (const gchar *)teco_view_ssm(ctx, SCI_GETRANGEPOINTER, gap, (sptr_t)size);
		gssize bytes_written = teco_eol_writer_convert(&writer, buffer, size, error);
		if (bytes_written < 0)
			return FALSE;
		g_assert(bytes_written == size);
	}

	return TRUE;
}

/** @memberof teco_view_t */
gboolean
teco_view_save_to_file(teco_view_t *ctx, const gchar *filename, GError **error)
{
#ifdef G_OS_UNIX
	GStatBuf file_stat;
	file_stat.st_uid = -1;
	file_stat.st_gid = -1;
#endif
	teco_file_attributes_t attributes = TECO_FILE_INVALID_ATTRIBUTES;

	if (teco_undo_enabled) {
		if (g_file_test(filename, G_FILE_TEST_IS_REGULAR)) {
#ifdef G_OS_UNIX
			g_stat(filename, &file_stat);
#endif
			attributes = teco_file_get_attributes(filename);
			teco_make_savepoint(filename);
		} else {
			teco_undo_remove_file_push(filename);
		}
	}

	/* leaves access mode intact if file still exists */
	g_autoptr(GIOChannel) channel = g_io_channel_new_file(filename, "w", error);
	if (!channel)
		return FALSE;

	/*
	 * teco_view_save_to_channel() expects a buffered and blocking channel
	 */
	g_io_channel_set_encoding(channel, NULL, NULL);
	g_io_channel_set_buffered(channel, TRUE);

	if (!teco_view_save_to_channel(ctx, channel, error)) {
		g_prefix_error(error, "Error writing file \"%s\": ", filename);
		return FALSE;
	}

	/* if file existed but has been renamed, restore attributes */
	if (attributes != TECO_FILE_INVALID_ATTRIBUTES)
		teco_file_set_attributes(filename, attributes);
#ifdef G_OS_UNIX
	/*
	 * only a good try to inherit owner since process user must have
	 * CHOWN capability traditionally reserved to root only.
	 * FIXME: We should probably fall back to another save point
	 * strategy.
	 */
	if (fchown(g_io_channel_unix_get_fd(channel),
	           file_stat.st_uid, file_stat.st_gid))
		teco_interface_msg(TECO_MSG_WARNING,
		                   "Unable to preserve owner of \"%s\": %s",
		                   filename, g_strerror(errno));
#endif

	return TRUE;
}
