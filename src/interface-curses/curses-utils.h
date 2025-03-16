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
#pragma once

#include <glib.h>

#include <curses.h>

guint teco_curses_format_str(WINDOW *win, const gchar *str, gsize len, gint max_width);

guint teco_curses_format_filename(WINDOW *win, const gchar *filename, gint max_width);

/**
 * Add Unicode character to window.
 * This is just like wadd_wch(), but does not require wide-char APIs.
 */
static inline void
teco_curses_add_wc(WINDOW *win, gunichar chr)
{
	gchar buf[6];
	waddnstr(win, buf, g_unichar_to_utf8(chr, buf));
}

/**
 * Clear from the current position until the end of the given
 * curses window with the current \b foreground attributes.
 * This is similar to wclrtobot(), but does not use the
 * background attributes.
 */
static inline void
teco_curses_clrtobot(WINDOW *win)
{
	int max_x, max_y;
	getmaxyx(win, max_y, max_x);
	if (getcurx(win)+1 < max_x)
		whline(win, ' ', max_x - getcurx(win));
	for (int y = getcury(win)+1; y <= max_y; y++)
		mvwhline(win, y, 0, ' ', max_x);
}
