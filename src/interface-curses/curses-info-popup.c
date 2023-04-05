/*
 * Copyright (C) 2012-2023 Robin Haberkorn
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

#include <glib.h>

#include <curses.h>

#include "list.h"
#include "string-utils.h"
#include "interface.h"
#include "curses-utils.h"
#include "curses-info-popup.h"

/*
 * FIXME: This is redundant with gtk-info-popup.c.
 */
typedef struct {
	teco_stailq_entry_t entry;

	teco_popup_entry_type_t type;
	teco_string_t name;
	gboolean highlight;
} teco_popup_entry_t;

void
teco_curses_info_popup_add(teco_curses_info_popup_t *ctx, teco_popup_entry_type_t type,
                           const gchar *name, gsize name_len, gboolean highlight)
{
	if (G_UNLIKELY(!ctx->chunk))
		ctx->chunk = g_string_chunk_new(32);

	/*
	 * FIXME: Test with g_slice_new()...
	 * It could however cause problems upon command-line termination
	 * and may not be measurably faster.
	 */
	teco_popup_entry_t *entry = g_new(teco_popup_entry_t, 1);
	entry->type = type;
	/*
	 * Popup entries aren't removed individually, so we can
	 * more efficiently store them via GStringChunk.
	 */
	teco_string_init_chunk(&entry->name, name, name_len, ctx->chunk);
	entry->highlight = highlight;

	teco_stailq_insert_tail(&ctx->list, &entry->entry);

	ctx->longest = MAX(ctx->longest, (gint)name_len);
	ctx->length++;
}

static void
teco_curses_info_popup_init_pad(teco_curses_info_popup_t *ctx, attr_t attr)
{
	int cols = getmaxx(stdscr);	/**! screen width */
	int pad_lines;			/**! pad height */
	gint pad_cols;			/**! entry columns */
	gint pad_colwidth;		/**! width per entry column */

	/* reserve 2 spaces between columns */
	pad_colwidth = MIN(ctx->longest + 2, cols - 2);

	/* pad_cols = floor((cols - 2) / pad_colwidth) */
	pad_cols = (cols - 2) / pad_colwidth;
	/* pad_lines = ceil(length / pad_cols) */
	pad_lines = (ctx->length+pad_cols-1) / pad_cols;

	/*
	 * Render the entire autocompletion list into a pad
	 * which can be higher than the physical screen.
	 * The pad uses two columns less than the screen since
	 * it will be drawn into the popup window which has left
	 * and right borders.
	 */
	ctx->pad = newpad(pad_lines, cols - 2);

	wbkgd(ctx->pad, ' ' | attr);

	/*
	 * cur_col is the row currently written.
	 * It does not wrap but grows indefinitely.
	 * Therefore the real current row is (cur_col % popup_cols)
	 */
	gint cur_col = 0;
	for (teco_stailq_entry_t *cur = ctx->list.first; cur != NULL; cur = cur->next) {
		teco_popup_entry_t *entry = (teco_popup_entry_t *)cur;
		gint cur_line = cur_col/pad_cols + 1;

		wmove(ctx->pad, cur_line-1,
		      (cur_col % pad_cols)*pad_colwidth);

		wattrset(ctx->pad, entry->highlight ? A_BOLD : A_NORMAL);

		switch (entry->type) {
		case TECO_POPUP_FILE:
		case TECO_POPUP_DIRECTORY:
			g_assert(!teco_string_contains(&entry->name, '\0'));
			teco_curses_format_filename(ctx->pad, entry->name.data, -1);
			break;
		default:
			teco_curses_format_str(ctx->pad, entry->name.data, entry->name.len, -1);
			break;
		}

		cur_col++;
	}
}

void
teco_curses_info_popup_show(teco_curses_info_popup_t *ctx, attr_t attr)
{
	if (!ctx->length)
		/* nothing to display */
		return;

	int lines, cols; /* screen dimensions */
	getmaxyx(stdscr, lines, cols);

	if (ctx->window)
		delwin(ctx->window);

	if (!ctx->pad)
		teco_curses_info_popup_init_pad(ctx, attr);
	gint pad_lines = getmaxy(ctx->pad);

	/*
	 * Popup window can cover all but one screen row.
	 * Another row is reserved for the top border.
	 */
	gint popup_lines = MIN(pad_lines + 1, lines - 1);

	/* window covers message, scintilla and info windows */
	ctx->window = newwin(popup_lines, 0, lines - 1 - popup_lines, 0);

	wbkgdset(ctx->window, ' ' | attr);

	wborder(ctx->window,
	        ACS_VLINE,
	        ACS_VLINE,	/* may be overwritten with scrollbar */
	        ACS_HLINE,
	        ' ',		/* no bottom line */
	        ACS_ULCORNER, ACS_URCORNER,
	        ACS_VLINE, ACS_VLINE);

	copywin(ctx->pad, ctx->window,
	        ctx->pad_first_line, 0,
	        1, 1, popup_lines - 1, cols - 2, FALSE);

	if (pad_lines <= popup_lines - 1)
		/* no need for scrollbar */
		return;

	/* bar_height = ceil((popup_lines-1)/pad_lines * (popup_lines-2)) */
	gint bar_height = ((popup_lines-1)*(popup_lines-2) + pad_lines-1) /
	                  pad_lines;
	/* bar_y = floor(pad_first_line/pad_lines * (popup_lines-2)) + 1 */
	gint bar_y = ctx->pad_first_line*(popup_lines-2) / pad_lines + 1;

	mvwvline(ctx->window, 1, cols-1, ACS_CKBOARD, popup_lines-2);
	/*
	 * We do not use ACS_BLOCK here since it will not
	 * always be drawn as a solid block (e.g. xterm).
	 * Instead, simply draw reverse blanks.
	 */
	wmove(ctx->window, bar_y, cols-1);
	wattron(ctx->window, A_REVERSE);
	wvline(ctx->window, ' ', bar_height);

	/* progress scroll position */
	ctx->pad_first_line += popup_lines - 1;
	/* wrap on last shown page */
	ctx->pad_first_line %= pad_lines;
	if (pad_lines - ctx->pad_first_line < popup_lines - 1)
		/* show last page */
		ctx->pad_first_line = pad_lines - (popup_lines - 1);
}

void
teco_curses_info_popup_clear(teco_curses_info_popup_t *ctx)
{
	if (ctx->window)
		delwin(ctx->window);
	if (ctx->pad)
		delwin(ctx->pad);
	if (ctx->chunk)
		g_string_chunk_free(ctx->chunk);

	teco_stailq_entry_t *entry;
	while ((entry = teco_stailq_remove_head(&ctx->list)))
		g_free(entry);

	teco_curses_info_popup_init(ctx);
}
