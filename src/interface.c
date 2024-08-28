/*
 * Copyright (C) 2012-2024 Robin Haberkorn
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

/**
 * Print a message to the appropriate stdio streams.
 *
 * This method has similar semantics to `vprintf`, i.e.
 * it leaves `ap` undefined. Therefore to pass the format
 * string and arguments to another `vprintf`-like function,
 * you have to copy the arguments via `va_copy`.
 */
void
teco_interface_stdio_vmsg(teco_msg_t type, const gchar *fmt, va_list ap)
{
	FILE *stream = stdout;

	switch (type) {
	case TECO_MSG_USER:
		break;
	case TECO_MSG_INFO:
		fputs("Info: ", stream);
		break;
	case TECO_MSG_WARNING:
		stream = stderr;
		fputs("Warning: ", stream);
		break;
	case TECO_MSG_ERROR:
		stream = stderr;
		fputs("Error: ", stream);
		break;
	}

	g_vfprintf(stream, fmt, ap);
	fputc('\n', stream);
}

void
teco_interface_process_notify(SCNotification *notify)
{
#ifdef DEBUG
	g_printf("SCINTILLA NOTIFY: code=%d\n", notify->nmhdr.code);
#endif
}

/**
 * Convert a glyph index to a byte offset as used by Scintilla.
 *
 * This is optimized with the "line character index",
 * which must always be enabled in UTF-8 documents.
 *
 * It is also used to validate glyph indexes.
 *
 * @param pos Position in glyphs/characters.
 * @return Position in bytes or -1 if pos is out of bounds.
 */
gssize
teco_glyphs2bytes(teco_int_t pos)
{
	if (pos < 0)
		return -1; /* invalid position */
	if (!pos)
		return 0;

	if (!(teco_interface_ssm(SCI_GETLINECHARACTERINDEX, 0, 0) &
	      SC_LINECHARACTERINDEX_UTF32))
		/* assume single-byte encoding */
		return pos <= teco_interface_ssm(SCI_GETLENGTH, 0, 0) ? pos : -1;

	sptr_t line = teco_interface_ssm(SCI_LINEFROMINDEXPOSITION, pos,
	                                 SC_LINECHARACTERINDEX_UTF32);
	sptr_t line_bytes = teco_interface_ssm(SCI_POSITIONFROMLINE, line, 0);
	pos -= teco_interface_ssm(SCI_INDEXPOSITIONFROMLINE, line,
	                          SC_LINECHARACTERINDEX_UTF32);
	return teco_interface_ssm(SCI_POSITIONRELATIVE, line_bytes, pos) ? : -1;
}

/**
 * Convert byte offset to glyph/character index without bounds checking.
 */
teco_int_t
teco_bytes2glyphs(gsize pos)
{
	if (!pos)
		return 0;

	if (!(teco_interface_ssm(SCI_GETLINECHARACTERINDEX, 0, 0) &
	      SC_LINECHARACTERINDEX_UTF32))
		/* assume single-byte encoding */
		return pos;

	sptr_t line = teco_interface_ssm(SCI_LINEFROMPOSITION, pos, 0);
	sptr_t line_bytes = teco_interface_ssm(SCI_POSITIONFROMLINE, line, 0);
	return teco_interface_ssm(SCI_INDEXPOSITIONFROMLINE, line,
	                          SC_LINECHARACTERINDEX_UTF32) +
	       teco_interface_ssm(SCI_COUNTCHARACTERS, line_bytes, pos);
}

#define TECO_RELATIVE_LIMIT 1024

/**
 * Convert a glyph index relative to a byte position to
 * a byte position.
 *
 * Can be used to implement commands with relative character
 * ranges.
 * As an optimization, this always counts characters for deltas
 * smaller than TECO_RELATIVE_LIMIT, so it will be fast
 * even where the character-index based lookup is too slow
 * (as on exceedingly long lines).
 *
 * @param pos Byte position to start.
 * @param n Number of glyphs/characters to the left (negative) or
 *   right (positive) of pos.
 * @return Position in bytes or -1 if the resulting position is out of bounds.
 */
gssize
teco_glyphs2bytes_relative(gsize pos, teco_int_t n)
{
	if (!n)
		return pos;
	if (ABS(n) > TECO_RELATIVE_LIMIT)
		return teco_glyphs2bytes(teco_bytes2glyphs(pos) + n);

	sptr_t res = teco_interface_ssm(SCI_POSITIONRELATIVE, pos, n);
	/* SCI_POSITIONRELATIVE may return 0 even if the offset is valid */
	return res ? : n > 0 ? -1 : teco_bytes2glyphs(pos)+n >= 0 ? 0 : -1;
}
