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
#include "string-utils.h"

typedef guint32 teco_file_attributes_t;
#define TECO_FILE_INVALID_ATTRIBUTES G_MAXUINT32

teco_file_attributes_t teco_file_get_attributes(const gchar *filename);
void teco_file_set_attributes(const gchar *filename, teco_file_attributes_t attrs);

/**
 * Get absolute/full version of a possibly relative path.
 * The path is tried to be canonicalized so it does
 * not contain relative components.
 * Works with existing and non-existing paths (in the latter case,
 * heuristics may be applied).
 * Depending on platform and existence of the path,
 * canonicalization might fail, but the path returned is
 * always absolute.
 *
 * @param path Possibly relative path name.
 * @return Newly-allocated absolute path name.
 */
gchar *teco_file_get_absolute_path(const gchar *path);

/**
 * Normalize path or file name.
 *
 * This changes the directory separators
 * to forward slash (on platforms that support
 * different directory separator styles).
 *
 * @param path The path to normalize.
 *             It is changed in place.
 * @return Returns `path`. The return value
 *         may be ignored.
 */
static inline gchar *
teco_file_normalize_path(gchar *path)
{
#if G_DIR_SEPARATOR != '/'
	return g_strdelimit(path, G_DIR_SEPARATOR_S, '/');
#else
	return path;
#endif
}

gboolean teco_file_is_visible(const gchar *path);

gchar *teco_file_expand_path(const gchar *path);

/**
 * This gets the length of a file name's directory
 * component including any trailing directory separator.
 * It returns 0 if the file name does not have a directory
 * separator.
 * This is useful when constructing file names in the same
 * directory as an existing one, keeping the exact same
 * directory component (globbing, tab completion...).
 * Also if it returns non-0, this can be used to look up
 * the last used directory separator in the file name.
 */
static inline gsize
teco_file_get_dirname_len(const gchar *path)
{
	gsize len = 0;

	for (const gchar *p = path; *p; p++)
		if (G_IS_DIR_SEPARATOR(*p))
			len = p - path + 1;

	return len;
}

static inline gboolean
teco_file_is_dir(const gchar *filename)
{
	if (!*filename)
		return FALSE;

	gchar c = filename[strlen(filename)-1];
	return G_IS_DIR_SEPARATOR(c);
}

gboolean teco_file_auto_complete(const gchar *filename, GFileTest file_test, teco_string_t *insert);
