/*
 * Copyright (C) 2012-2015 Robin Haberkorn
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

#include "string-utils.h"

namespace SciTECO {

void
String::get_coord(const gchar *str, gint pos,
                  gint &line, gint &column)
{
	line = column = 1;

	for (gint i = 0; i < pos; i++) {
		switch (str[i]) {
		case '\r':
			if (str[i+1] == '\n')
				i++;
			/* fall through */
		case '\n':
			line++;
			column = 1;
			break;
		default:
			column++;
			break;
		}
	}
}

} /* namespace SciTECO */
