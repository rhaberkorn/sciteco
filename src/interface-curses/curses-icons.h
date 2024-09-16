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
#pragma once

#include <glib.h>

/**
 * Q-Register icon.
 * 0xf04cf would look more similar to the current Gtk icon.
 */
#define TECO_CURSES_ICONS_QREG 0xe236 /*  */

gunichar teco_curses_icons_lookup_file(const gchar *filename);
gunichar teco_curses_icons_lookup_dir(const gchar *dirname);
