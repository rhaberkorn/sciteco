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

#include <string.h>

#include <glib.h>

#include <curses.h>

#include "curses-utils.h"
#include "curses-info-popup.h"

namespace SciTECO {

void
CursesInfoPopup::add(PopupEntryType type,
                     const gchar *name, bool highlight)
{
	size_t name_len = strlen(name);
	Entry *entry = (Entry *)g_malloc(sizeof(Entry) + name_len + 1);

	entry->type = type;
	entry->highlight = highlight;
	strcpy(entry->name, name);

	longest = MAX(longest, (gint)name_len);
	length++;

	/*
	 * Entries are added in reverse (constant time for GSList),
	 * so they will later have to be reversed.
	 */
	list = g_slist_prepend(list, entry);
}

void
CursesInfoPopup::init_pad(attr_t attr)
{
	int cols = getmaxx(stdscr);	/* screen width */
	int pad_lines;			/* pad height */
	gint pad_cols;			/* entry columns */
	gint pad_colwidth;		/* width per entry column */

	gint cur_col;

	/* reserve 2 spaces between columns */
	pad_colwidth = MIN(longest + 2, cols - 2);

	/* pad_cols = floor((cols - 2) / pad_colwidth) */
	pad_cols = (cols - 2) / pad_colwidth;
	/* pad_lines = ceil(length / pad_cols) */
	pad_lines = (length+pad_cols-1) / pad_cols;

	/*
	 * Render the entire autocompletion list into a pad
	 * which can be higher than the physical screen.
	 * The pad uses two columns less than the screen since
	 * it will be drawn into the popup window which has left
	 * and right borders.
	 */
	pad = newpad(pad_lines, cols - 2);

	wbkgd(pad, ' ' | attr);

	/*
	 * cur_col is the row currently written.
	 * It does not wrap but grows indefinitely.
	 * Therefore the real current row is (cur_col % popup_cols)
	 */
	cur_col = 0;
	for (GSList *cur = list; cur != NULL; cur = g_slist_next(cur)) {
		Entry *entry = (Entry *)cur->data;
		gint cur_line = cur_col/pad_cols + 1;

		wmove(pad, cur_line-1,
		      (cur_col % pad_cols)*pad_colwidth);

		wattrset(pad, entry->highlight ? A_BOLD : A_NORMAL);

		switch (entry->type) {
		case POPUP_FILE:
		case POPUP_DIRECTORY:
			Curses::format_filename(pad, entry->name);
			break;
		default:
			Curses::format_str(pad, entry->name);
			break;
		}

		cur_col++;
	}
}

void
CursesInfoPopup::show(attr_t attr)
{
	int lines, cols; /* screen dimensions */
	gint pad_lines;
	gint popup_lines;
	gint bar_height, bar_y;

	if (!length)
		/* nothing to display */
		return;

	getmaxyx(stdscr, lines, cols);

	if (window)
		delwin(window);
	else
		/* reverse list only once */
		list = g_slist_reverse(list);

	if (!pad)
		init_pad(attr);
	pad_lines = getmaxy(pad);

	/*
	 * Popup window can cover all but one screen row.
	 * Another row is reserved for the top border.
	 */
	popup_lines = MIN(pad_lines + 1, lines - 1);

	/* window covers message, scintilla and info windows */
	window = newwin(popup_lines, 0, lines - 1 - popup_lines, 0);

	wbkgdset(window, ' ' | attr);

	wborder(window,
	        ACS_VLINE,
	        ACS_VLINE,	/* may be overwritten with scrollbar */
	        ACS_HLINE,
	        ' ',		/* no bottom line */
	        ACS_ULCORNER, ACS_URCORNER,
	        ACS_VLINE, ACS_VLINE);

	copywin(pad, window,
	        pad_first_line, 0,
	        1, 1, popup_lines - 1, cols - 2, FALSE);

	if (pad_lines <= popup_lines - 1)
		/* no need for scrollbar */
		return;

	/* bar_height = ceil((popup_lines-1)/pad_lines * (popup_lines-2)) */
	bar_height = ((popup_lines-1)*(popup_lines-2) + pad_lines-1) /
	             pad_lines;
	/* bar_y = floor(pad_first_line/pad_lines * (popup_lines-2)) + 1 */
	bar_y = pad_first_line*(popup_lines-2) / pad_lines + 1;

	mvwvline(window, 1, cols-1, ACS_CKBOARD, popup_lines-2);
	/*
	 * We do not use ACS_BLOCK here since it will not
	 * always be drawn as a solid block (e.g. xterm).
	 * Instead, simply draw reverse blanks.
	 */
	wmove(window, bar_y, cols-1);
	wattron(window, A_REVERSE);
	wvline(window, ' ', bar_height);

	/* progress scroll position */
	pad_first_line += popup_lines - 1;
	/* wrap on last shown page */
	pad_first_line %= pad_lines;
	if (pad_lines - pad_first_line < popup_lines - 1)
		/* show last page */
		pad_first_line = pad_lines - (popup_lines - 1);
}

void
CursesInfoPopup::clear(void)
{
	g_slist_free_full(list, g_free);
	list = NULL;
	length = 0;
	longest = 0;

	pad_first_line = 0;

	if (window) {
		delwin(window);
		window = NULL;
	}

	if (pad) {
		delwin(pad);
		pad = NULL;
	}
}

CursesInfoPopup::~CursesInfoPopup()
{
	if (window)
		delwin(window);
	if (pad)
		delwin(pad);
	if (list)
		g_slist_free_full(list, g_free);
}

} /* namespace SciTECO */
