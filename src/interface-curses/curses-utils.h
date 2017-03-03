/*
 * Copyright (C) 2012-2017 Robin Haberkorn
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

#ifndef __CURSES_UTILS_H
#define __CURSES_UTILS_H

#include <glib.h>

#include <curses.h>

namespace SciTECO {

namespace Curses {

gsize format_str(WINDOW *win, const gchar *str,
                 gssize len = -1, gint max_width = -1);

gsize format_filename(WINDOW *win, const gchar *filename,
                      gint max_width = -1);

} /* namespace Curses */

} /* namespace SciTECO */

#endif
