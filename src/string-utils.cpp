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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib.h>

#include "sciteco.h"
#include "string-utils.h"

namespace SciTECO {

/**
 * Canonicalize control characters in str.
 * This converts all control characters to printable
 * characters without tabs, line feeds, etc.
 * Useful for displaying Q-Register names and
 * TECO code.
 */
gchar *
String::canonicalize_ctl(const gchar *str)
{
	gsize ret_len = 1; /* for trailing 0 */
	gchar *ret, *p;

	/*
	 * Instead of approximating size with strlen()
	 * we can just as well calculate it exactly:
	 */
	for (const gchar *p = str; *p; p++)
		ret_len += IS_CTL(*p) ? 2 : 1;

	p = ret = (gchar *)g_malloc(ret_len);

	while (*str) {
		if (IS_CTL(*str)) {
			*p++ = '^';
			*p++ = CTL_ECHO(*str++);
		} else {
			*p++ = *str++;
		}
	}
	*p = '\0';

	return ret;
}

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
