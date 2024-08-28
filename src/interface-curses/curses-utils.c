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

#include <string.h>

#include <glib.h>

#include <curses.h>

#include "sciteco.h"
#include "string-utils.h"
#include "curses-utils.h"

/**
 * Render UTF-8 string with TECO character representations.
 *
 * Strings are cut off with `...` at the end if necessary.
 * The mapping is similar to teco_view_set_representations().
 *
 * @param win The Curses window to write to.
 * @param str The string to format.
 * @param len The length of the string in bytes.
 * @param max_width The maximum width to consume in
 *   the window in characters. If smaller 0, take the
 *   entire remaining space in the window.
 * @return Number of characters actually written.
 */
guint
teco_curses_format_str(WINDOW *win, const gchar *str, gsize len, gint max_width)
{
	int old_x, old_y;
	gint chars_added = 0;

	getyx(win, old_y, old_x);

	if (max_width < 0)
		max_width = getmaxx(win) - old_x;

	while (len > 0) {
		/*
		 * NOTE: It shouldn't be possible to meet any string,
		 * that is not valid UTF-8.
		 */
		gsize clen = g_utf8_next_char(str) - str;

		/*
		 * NOTE: This mapping is similar to
		 * teco_view_set_representations().
		 */
		switch (*str) {
		case '\e':
			chars_added++;
			if (chars_added > max_width)
				goto truncate;
			waddch(win, '$' | A_REVERSE);
			break;
		case '\r':
			chars_added += 2;
			if (chars_added > max_width)
				goto truncate;
			waddch(win, 'C' | A_REVERSE);
			waddch(win, 'R' | A_REVERSE);
			break;
		case '\n':
			chars_added += 2;
			if (chars_added > max_width)
				goto truncate;
			waddch(win, 'L' | A_REVERSE);
			waddch(win, 'F' | A_REVERSE);
			break;
		case '\t':
			chars_added += 3;
			if (chars_added > max_width)
				goto truncate;
			waddch(win, 'T' | A_REVERSE);
			waddch(win, 'A' | A_REVERSE);
			waddch(win, 'B' | A_REVERSE);
			break;
		default:
			if (TECO_IS_CTL(*str)) {
				chars_added += 2;
				if (chars_added > max_width)
					goto truncate;
				waddch(win, '^' | A_REVERSE);
				waddch(win, TECO_CTL_ECHO(*str) | A_REVERSE);
			} else {
				chars_added++;
				if (chars_added > max_width)
					goto truncate;
				/*
				 * FIXME: This works with UTF-8 on ncurses,
				 * since it detects multi-byte characters.
				 * However on other platforms wadd_wch() may be
				 * necessary, which requires a widechar Curses variant.
				 */
				waddnstr(win, str, clen);
			}
		}

		str += clen;
		len -= clen;
	}

	return getcurx(win) - old_x;

truncate:
	if (max_width >= 3) {
		/*
		 * Truncate string
		 */
		wattron(win, A_UNDERLINE | A_BOLD);
		mvwaddstr(win, old_y, old_x + max_width - 3, "...");
		wattroff(win, A_UNDERLINE | A_BOLD);
	}

	return getcurx(win) - old_x;
}

/**
 * Render UTF-8 filename.
 *
 * This cuts of overlong filenames with `...` at the beginning,
 * possibly skipping any drive letter.
 * Control characters are escaped, but not highlighted.
 *
 * @param win The Curses window to write to.
 * @param filename Null-terminated filename to render.
 * @param max_width The maximum width to consume in
 *   the window in characters. If smaller 0, take the
 *   entire remaining space in the window.
 * @return Number of characters actually written.
 */
guint
teco_curses_format_filename(WINDOW *win, const gchar *filename, gint max_width)
{
	int old_x = getcurx(win);

	g_autofree gchar *filename_printable = teco_string_echo(filename, strlen(filename));
	glong filename_len = g_utf8_strlen(filename_printable, -1);

	if (max_width < 0)
		max_width = getmaxx(win) - old_x;

	if (filename_len <= max_width) {
		/*
		 * FIXME: This works with UTF-8 on ncurses,
		 * since it detects multi-byte characters.
		 * However on other platforms wadd_wch() may be
		 * necessary, which requires a widechar Curses variant.
		 */
		waddstr(win, filename_printable);
	} else if (filename_len >= 3) {
		const gchar *keep_post;
		keep_post = g_utf8_offset_to_pointer(filename_printable + strlen(filename_printable),
		                                     -max_width + 3);

#ifdef G_OS_WIN32
		const gchar *keep_pre = g_path_skip_root(filename_printable);
		if (keep_pre) {
			waddnstr(win, filename_printable,
			         keep_pre - filename_printable);
			keep_post += keep_pre - filename_printable;
		}
#endif
		wattron(win, A_UNDERLINE | A_BOLD);
		waddstr(win, "...");
		wattroff(win, A_UNDERLINE | A_BOLD);
		waddstr(win, keep_post);
	}

	return getcurx(win) - old_x;
}
