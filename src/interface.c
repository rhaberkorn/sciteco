/*
 * Copyright (C) 2012-2025 Robin Haberkorn
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

#include <stdarg.h>
#include <stdio.h>

#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>

#include <Scintilla.h>

#include "sciteco.h"
#include "string-utils.h"
#include "undo.h"
#include "view.h"
#include "interface.h"

//#define DEBUG

teco_view_t *teco_interface_current_view = NULL;

TECO_DEFINE_UNDO_CALL(teco_interface_show_view, teco_view_t *);
TECO_DEFINE_UNDO_CALL(teco_interface_ssm, unsigned int, uptr_t, sptr_t);
TECO_DEFINE_UNDO_CALL(teco_interface_info_update_qreg, const teco_qreg_t *);
TECO_DEFINE_UNDO_CALL(teco_interface_info_update_buffer, const teco_buffer_t *);

/** Last mouse event */
teco_mouse_t teco_mouse = {0};

typedef struct {
	teco_string_t str;
	gchar name[];
} teco_undo_set_clipboard_t;

static void
teco_undo_set_clipboard_action(teco_undo_set_clipboard_t *ctx, gboolean run)
{
	if (run)
		teco_interface_set_clipboard(ctx->name, ctx->str.data, ctx->str.len, NULL);
	teco_string_clear(&ctx->str);
}

/**
 * Set the clipboard upon rubout.
 *
 * This passes ownership of the clipboard content string
 * to the undo token object.
 */
void
teco_interface_undo_set_clipboard(const gchar *name, gchar *str, gsize len)
{
	teco_undo_set_clipboard_t *ctx;

	ctx = teco_undo_push_size((teco_undo_action_t)teco_undo_set_clipboard_action,
	                          sizeof(*ctx) + strlen(name) + 1);
	if (ctx) {
		ctx->str.data = str;
		ctx->str.len = len;
		strcpy(ctx->name, name);
	} else {
		g_free(str);
	}
}

void
teco_interface_msg(teco_msg_t type, const gchar *fmt, ...)
{
	gchar buf[512];
	va_list ap;

	va_start(ap, fmt);
	/*
	 * If the buffer could ever be exceeded, perhaps
	 * use g_strdup_vprintf() instead.
	 */
	gint len = g_vsnprintf(buf, sizeof(buf), fmt, ap);
	g_assert(0 <= len && len < sizeof(buf));
	va_end(ap);

	teco_interface_msg_literal(type, buf, len);
}

/**
 * Print a raw message to the appropriate stdio streams.
 *
 * This deliberately does not echo (i.e. escape non-printable characters)
 * the string. Either they are supposed to be written verbatim
 * (TECO_MSG_USER) or are already echoed.
 * Everything higher than TECO_MSG_USER is also terminated by LF.
 *
 * @fixme TECO_MSG_USER could always be flushed.
 * This however makes the message disappear, though.
 * We might also want to put flushing under control of the language instead.
 */
void
teco_interface_stdio_msg(teco_msg_t type, const gchar *str, gsize len)
{
	switch (type) {
	case TECO_MSG_USER:
		fwrite(str, 1, len, stdout);
		//fflush(stdout);
		break;
	case TECO_MSG_INFO:
		g_fprintf(stdout, "Info: %.*s\n", (gint)len, str);
		break;
	case TECO_MSG_WARNING:
		g_fprintf(stderr, "Warning: %.*s\n", (gint)len, str);
		break;
	case TECO_MSG_ERROR:
		g_fprintf(stderr, "Error: %.*s\n", (gint)len, str);
		break;
	}
}

/**
 * Get character from stdin.
 *
 * @param widechar If TRUE reads one glyph encoded in UTF-8.
 *   If FALSE, returns exactly one byte.
 * @return Codepoint or -1 in case of EOF.
 */
teco_int_t
teco_interface_stdio_getch(gboolean widechar)
{
	gchar buf[4];
	gint i = 0;
	gint32 cp;

	do {
		if (G_UNLIKELY(fread(buf+i, 1, 1, stdin) < 1))
			return -1; /* EOF */
		if (!widechar || !buf[i])
			return (guchar)buf[i];

		/* doesn't work as expected when passed a null byte */
		cp = g_utf8_get_char_validated(buf, ++i);
		if (i >= sizeof(buf) || cp != -2)
			i = 0;
	} while (cp < 0);

	return cp;
}
