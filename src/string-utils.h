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

#ifndef __STRING_UTILS_H
#define __STRING_UTILS_H

#include <string.h>

#include <glib.h>

namespace SciTECO {

namespace String {

/**
 * Allocate a string containing a single character chr.
 */
static inline gchar *
chrdup(gchar chr)
{
	gchar *ret = (gchar *)g_malloc(2);

	ret[0] = chr;
	ret[1] = '\0';

	return ret;
}

/**
 * Append null-terminated str2 to non-null-terminated
 * str1 of length str1_size.
 * The result is not null-terminated.
 */
static inline void
append(gchar *&str1, gsize str1_size, const gchar *str2)
{
	size_t str2_size = strlen(str2);
	str1 = (gchar *)g_realloc(str1, str1_size + str2_size);
	if (str1)
		memcpy(str1+str1_size, str2, str2_size);
}

/**
 * Append str2 to str1 (both null-terminated).
 */
static inline void
append(gchar *&str1, const gchar *str2)
{
	size_t str1_size = str1 ? strlen(str1) : 0;
	str1 = (gchar *)g_realloc(str1, str1_size + strlen(str2) + 1);
	strcpy(str1+str1_size, str2);
}

/**
 * Append a single character to a null-terminated string.
 */
static inline void
append(gchar *&str, gchar chr)
{
	gchar buf[] = {chr, '\0'};
	append(str, buf);
}

gchar *canonicalize_ctl(const gchar *str);

void get_coord(const gchar *str, gint pos,
	       gint &line, gint &column);

static inline gsize
diff(const gchar *a, const gchar *b)
{
	gsize len = 0;

	while (*a != '\0' && *a++ == *b++)
		len++;

	return len;
}

} /* namespace String */

} /* namespace SciTECO */

#endif
