/*
 * Copyright (C) 2012-2021 Robin Haberkorn
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
#include "undo.h"
#include "string-utils.h"

/**
 * Get echoable (printable) version of a given string.
 *
 * This converts all control characters to printable
 * characters without tabs, line feeds, etc.
 * That's also why it can safely return a null-terminated string.
 * Useful for displaying Q-Register names and TECO code.
 *
 * @memberof teco_string_t
 */
gchar *
teco_string_echo(const gchar *str, gsize len)
{
	gchar *ret, *p;

	p = ret = g_malloc(len*2 + 1);

	for (guint i = 0; i < len; i++) {
		if (TECO_IS_CTL(str[i])) {
			*p++ = '^';
			*p++ = TECO_CTL_ECHO(str[i]);
		} else {
			*p++ = str[i];
		}
	}
	*p = '\0';

	return ret;
}

/** @memberof teco_string_t */
void
teco_string_get_coord(const gchar *str, guint pos, guint *line, guint *column)
{
	*line = *column = 1;

	for (guint i = 0; i < pos; i++) {
		switch (str[i]) {
		case '\r':
			if (str[i+1] == '\n')
				i++;
			/* fall through */
		case '\n':
			(*line)++;
			(*column) = 1;
			break;
		default:
			(*column)++;
			break;
		}
	}
}

/** @memberof teco_string_t */
gsize
teco_string_diff(const teco_string_t *a, const gchar *b, gsize b_len)
{
	gsize len = 0;

	while (len < a->len && len < b_len &&
	       a->data[len] == b[len])
		len++;

	return len;
}

/** @memberof teco_string_t */
gsize
teco_string_casediff(const teco_string_t *a, const gchar *b, gsize b_len)
{
	gsize len = 0;

	while (len < a->len && len < b_len &&
	       g_ascii_tolower(a->data[len]) == g_ascii_tolower(b[len]))
		len++;

	return len;
}

/** @memberof teco_string_t */
gint
teco_string_cmp(const teco_string_t *a, const gchar *b, gsize b_len)
{
	for (guint i = 0; i < a->len; i++) {
		if (i == b_len)
			/* b is a prefix of a */
			return 1;
		gint ret = (gint)a->data[i] - (gint)b[i];
		if (ret != 0)
			/* a and b have a common prefix of length i */
			return ret;
	}

	return a->len == b_len ? 0 : -1;
}

/** @memberof teco_string_t */
gint
teco_string_casecmp(const teco_string_t *a, const gchar *b, gsize b_len)
{
	for (guint i = 0; i < a->len; i++) {
		if (i == b_len)
			/* b is a prefix of a */
			return 1;
		gint ret = (gint)g_ascii_tolower(a->data[i]) - (gint)g_ascii_tolower(b[i]);
		if (ret != 0)
			/* a and b have a common prefix of length i */
			return ret;
	}

	return a->len == b_len ? 0 : -1;
}

/**
 * Find string after the last occurrence of any in a set of characters.
 *
 * @param str String to search through.
 * @param chars Null-terminated set of characters.
 *              The null-byte itself is always considered part of the set.
 * @return A null-terminated suffix of str or NULL.
 *
 * @memberof teco_string_t
 */
const gchar *
teco_string_last_occurrence(const teco_string_t *str, const gchar *chars)
{
	teco_string_t ret = *str;

	if (!ret.len)
		return NULL;

	do {
		gint i = teco_string_rindex(&ret, *chars);
		if (i >= 0) {
			ret.data += i+1;
			ret.len -= i+1;
		}
	} while (*chars++);

	return ret.data;
}

TECO_DEFINE_UNDO_CALL(teco_string_truncate, teco_string_t *, gsize);

TECO_DEFINE_UNDO_OBJECT(cstring, gchar *, g_strdup, g_free);

static inline teco_string_t
teco_string_copy(const teco_string_t str)
{
	teco_string_t ret;
	teco_string_init(&ret, str.data, str.len);
	return ret;
}

#define DELETE(X) teco_string_clear(&(X))
TECO_DEFINE_UNDO_OBJECT(string, teco_string_t, teco_string_copy, DELETE);
TECO_DEFINE_UNDO_OBJECT_OWN(string_own, teco_string_t, DELETE);
#undef DELETE
