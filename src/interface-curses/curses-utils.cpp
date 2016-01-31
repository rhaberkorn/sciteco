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

#include "sciteco.h"
#include "string-utils.h"
#include "curses-utils.h"

namespace SciTECO {

gsize
Curses::format_str(WINDOW *win, const gchar *str,
                   gssize len, gint max_width)
{
	int old_x, old_y;
	gint chars_added = 0;

	getyx(win, old_y, old_x);

	if (len < 0)
		len = strlen(str);
	if (max_width < 0)
		max_width = getmaxx(win) - old_x;

	while (len > 0) {
		/*
		 * NOTE: This mapping is similar to
		 * View::set_representations()
		 */
		switch (*str) {
		case CTL_KEY_ESC:
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
			if (IS_CTL(*str)) {
				chars_added += 2;
				if (chars_added > max_width)
					goto truncate;
				waddch(win, '^' | A_REVERSE);
				waddch(win, CTL_ECHO(*str) | A_REVERSE);
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
Curses::format_filename(WINDOW *win, const gchar *filename,
                        gint max_width)
{
	int old_x = getcurx(win);

	gchar *filename_canon = String::canonicalize_ctl(filename);
	size_t filename_len = strlen(filename_canon);

	if (max_width < 0)
		max_width = getmaxx(win) - old_x;

	if (filename_len <= (size_t)max_width) {
		waddstr(win, filename_canon);
	} else {
		const gchar *keep_post = filename_canon + filename_len -
		                         max_width + 3;

#ifdef G_OS_WIN32
		const gchar *keep_pre = g_path_skip_root(filename_canon);
		if (keep_pre) {
			waddnstr(win, filename_canon,
			         keep_pre - filename_canon);
			keep_post += keep_pre - filename_canon;
		}
#endif
		wattron(win, A_UNDERLINE | A_BOLD);
		waddstr(win, "...");
		wattroff(win, A_UNDERLINE | A_BOLD);
		waddstr(win, keep_post);
	}

	g_free(filename_canon);
	return getcurx(win) - old_x;
}

} /* namespace SciTECO */
