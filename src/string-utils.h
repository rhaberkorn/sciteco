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

#include <string.h>

#include <glib.h>

#include "sciteco.h"
#include "undo.h"

/**
 * Upper-case SciTECO command character.
 *
 * There are implementations in glib (g_ascii_toupper() and g_unichar_toupper()) and libc,
 * but this implementation is sufficient for all letters used by SciTECO commands.
 */
static inline gunichar
teco_ascii_toupper(gunichar chr)
{
	return chr >= 'a' && chr <= 'z' ? chr & ~0x20 : chr;
}

static inline gchar *
teco_strv_remove(gchar **strv, guint i)
{
	gchar *ret = strv[i];
	do
		strv[i] = strv[i+1];
	while (strv[++i]);
	return ret;
}

/**
 * An 8-bit clean null-terminated string.
 *
 * This is similar to GString, but the container does not need to be allocated
 * and the allocation length is not stored.
 * Just like GString, teco_string_t are always null-terminated but at the
 * same time 8-bit clean (can contain null-characters).
 * It may or may not contain UTF-8 byte sequences.
 *
 * The API is designed such that teco_string_t operations operate on plain
 * (null-terminated) C strings, a single character or character array as well as
 * on other teco_string_t.
 * Input strings will thus usually be specified using a const gchar * and gsize
 * and are not necessarily null-terminated.
 * A target teco_string_t::data is always null-terminated and thus safe to pass
 * to functions expecting traditional null-terminated C strings if you can
 * guarantee that it contains no null-character other than the trailing one.
 *
 * @warning For consistency with C idioms the underlying character type is
 * `char`, which might be signed!
 * Accessing individual characters may yield signed integers and that sign
 * might be preserved when upcasting to a larger signed integer.
 * In this case you should always cast to `guchar` first.
 */
typedef struct {
	/**
	 * g_malloc() or g_string_chunk_insert()-allocated null-terminated string.
	 * The pointer is guaranteed to be non-NULL after initialization.
	 */
	gchar *data;
	/** Length of `data` without the trailing null-byte in bytes. */
	gsize len;
} teco_string_t;

/** @memberof teco_string_t */
static inline void
teco_string_init(teco_string_t *target, const gchar *str, gsize len)
{
	target->data = g_malloc(len + 1);
	if (str)
		memcpy(target->data, str, len);
	target->len = len;
	target->data[target->len] = '\0';
}

/**
 * Allocate a teco_string_t using GStringChunk.
 *
 * Such strings must not be freed/cleared individually and it is NOT allowed
 * to call teco_string_append() and teco_string_truncate() on them.
 * On the other hand, they are stored faster and more memory efficient.
 *
 * @memberof teco_string_t
 */
static inline void
teco_string_init_chunk(teco_string_t *target, const gchar *str, gssize len, GStringChunk *chunk)
{
	target->data = g_string_chunk_insert_len(chunk, str, len);
	target->len = len;
}

/**
 * @note Rounding up the length turned out to bring no benefits,
 * at least with glibc's malloc().
 *
 * @memberof teco_string_t
 */
static inline void
teco_string_append(teco_string_t *target, const gchar *str, gsize len)
{
	target->data = g_realloc(target->data, target->len + len + 1);
	if (str)
		memcpy(target->data + target->len, str, len);
	target->len += len;
	target->data[target->len] = '\0';
}

/** @memberof teco_string_t */
static inline void
teco_string_append_c(teco_string_t *str, gchar chr)
{
	teco_string_append(str, &chr, sizeof(chr));
}

/** @memberof teco_string_t */
static inline void
teco_string_append_wc(teco_string_t *target, gunichar chr)
{
	/* 4 bytes should be enough, but we better follow the documentation */
	target->data = g_realloc(target->data, target->len + 6 + 1);
	target->len += g_unichar_to_utf8(chr, target->data+target->len);
	target->data[target->len] = '\0';
}

/**
 * @fixme Should this also realloc str->data?
 *
 * @memberof teco_string_t
 */
static inline void
teco_string_truncate(teco_string_t *str, gsize len)
{
	g_assert(len <= str->len);
	if (len) {
		str->data[len] = '\0';
	} else {
		g_free(str->data);
		str->data = NULL;
	}
	str->len = len;
}

/** @memberof teco_string_t */
void undo__teco_string_truncate(teco_string_t *, gsize);

gchar *teco_string_echo(const gchar *str, gsize len);

void teco_string_get_coord(const gchar *str, guint pos, guint *line, guint *column);

typedef gsize (*teco_string_diff_t)(const teco_string_t *a, const gchar *b, gsize b_len);
gsize teco_string_diff(const teco_string_t *a, const gchar *b, gsize b_len);
gsize teco_string_casediff(const teco_string_t *a, const gchar *b, gsize b_len);

typedef gint (*teco_string_cmp_t)(const teco_string_t *a, const gchar *b, gsize b_len);
gint teco_string_cmp(const teco_string_t *a, const gchar *b, gsize b_len);
gint teco_string_casecmp(const teco_string_t *a, const gchar *b, gsize b_len);

/** @memberof teco_string_t */
static inline gboolean
teco_string_contains(const teco_string_t *str, gchar chr)
{
	return str->data && memchr(str->data, chr, str->len);
}

/**
 * Get index of character in string.
 *
 * @return Index of character in string. 0 refers to the first character.
 *         In case of search failure, a negative value is returned.
 *
 * @memberof teco_string_t
 */
static inline gint
teco_string_rindex(const teco_string_t *str, gchar chr)
{
	gint i;
	for (i = str->len-1; i >= 0 && str->data[i] != chr; i--);
	return i;
}

const gchar *teco_string_last_occurrence(const teco_string_t *str, const gchar *chars);

/**
 * Validate whether string consists exclusively of valid UTF-8, but accept null bytes.
 * @note there is g_utf8_validate_len() in Glib 2.60
 */
static inline gboolean
teco_string_validate_utf8(const teco_string_t *str)
{
	const gchar *p = str->data;
	while (!g_utf8_validate(p, str->len - (p - str->data), &p) && !*p)
		p++;
	return p - str->data == str->len;
}

/** @memberof teco_string_t */
static inline void
teco_string_clear(teco_string_t *str)
{
	g_free(str->data);
}

TECO_DECLARE_UNDO_OBJECT(cstring, gchar *);

#define teco_undo_cstring(VAR) \
	(*teco_undo_object_cstring_push(&(VAR)))

TECO_DECLARE_UNDO_OBJECT(string, teco_string_t);

#define teco_undo_string(VAR) \
	(*teco_undo_object_string_push(&(VAR)))

TECO_DECLARE_UNDO_OBJECT(string_own, teco_string_t);

#define teco_undo_string_own(VAR) \
	(*teco_undo_object_string_own_push(&(VAR)))

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(teco_string_t, teco_string_clear);
