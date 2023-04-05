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

#include <string.h>

#include <glib.h>

#include <curses.h>

#include "sciteco.h"
#include "string-utils.h"
#include "curses-utils.h"

gsize
teco_curses_format_str(WINDOW *win, const gchar *str, gsize len, gint max_width)
{
	int old_x, old_y;
	gint chars_added = 0;

	getyx(win, old_y, old_x);

	if (max_width < 0)
		max_width = getmaxx(win) - old_x;

	while (len > 0) {
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
				waddch(win, *str);
			}
		}

		str++;
		len--;
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

gsize
teco_curses_format_filename(WINDOW *win, const gchar *filename,
                            gint max_width)
{
	int old_x = getcurx(win);

	g_autofree gchar *filename_printable = teco_string_echo(filename, strlen(filename));
	size_t filename_len = strlen(filename_printable);

	if (max_width < 0)
		max_width = getmaxx(win) - old_x;

	if (filename_len <= (size_t)max_width) {
		waddstr(win, filename_printable);
	} else {
		const gchar *keep_post = filename_printable + filename_len -
		                         max_width + 3;

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
