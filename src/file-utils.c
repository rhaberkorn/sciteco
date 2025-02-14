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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define _GNU_SOURCE
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef HAVE_WINDOWS_H
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#include <windows.h>
#endif

#include <glib.h>
#include <glib/gstdio.h>

#ifdef G_OS_UNIX
#include <dlfcn.h>
#endif

#include "sciteco.h"
#include "qreg.h"
#include "interface.h"
#include "string-utils.h"
#include "file-utils.h"

#ifdef G_OS_WIN32

/*
 * NOTE: File attributes are represented as DWORDs in the Win32 API
 * which should be equivalent to guint32.
 */
G_STATIC_ASSERT(sizeof(DWORD) == sizeof(teco_file_attributes_t));
/*
 * NOTE: Invalid file attributes should be represented by 0xFFFFFFFF.
 */
G_STATIC_ASSERT(INVALID_FILE_ATTRIBUTES == TECO_FILE_INVALID_ATTRIBUTES);

teco_file_attributes_t
teco_file_get_attributes(const gchar *filename)
{
	g_autofree gunichar2 *filename_utf16 = g_utf8_to_utf16(filename, -1, NULL, NULL, NULL);
	return filename_utf16 ? GetFileAttributesW(filename_utf16)
	                      : TECO_FILE_INVALID_ATTRIBUTES;
}

void
teco_file_set_attributes(const gchar *filename, teco_file_attributes_t attrs)
{
	g_autofree gunichar2 *filename_utf16 = g_utf8_to_utf16(filename, -1, NULL, NULL, NULL);
	if (filename_utf16)
		SetFileAttributesW(filename_utf16, attrs);
}

gchar *
teco_file_get_absolute_path(const gchar *path)
{
	if (!path)
		return NULL;
	g_autofree gunichar2 *path_utf16 = g_utf8_to_utf16(path, -1, NULL, NULL, NULL);
	TCHAR buf[MAX_PATH];
	return path_utf16 && GetFullPathNameW(path_utf16, G_N_ELEMENTS(buf), buf, NULL)
					? g_utf16_to_utf8(buf, -1, NULL, NULL, NULL) : NULL;
}

gboolean
teco_file_is_visible(const gchar *path)
{
	g_autofree gunichar2 *path_utf16 = g_utf8_to_utf16(path, -1, NULL, NULL, NULL);
	return path_utf16 && !(GetFileAttributesW(path_utf16) & FILE_ATTRIBUTE_HIDDEN);
}

#else /* !G_OS_WIN32 */

teco_file_attributes_t
teco_file_get_attributes(const gchar *filename)
{
	GStatBuf buf;
	return g_stat(filename, &buf) ? TECO_FILE_INVALID_ATTRIBUTES : buf.st_mode;
}

void
teco_file_set_attributes(const gchar *filename, teco_file_attributes_t attrs)
{
	g_chmod(filename, attrs);
}

#ifdef G_OS_UNIX

gchar *
teco_file_get_absolute_path(const gchar *path)
{
	gchar buf[PATH_MAX];

	if (!path)
		return NULL;
	if (realpath(path, buf))
		return g_strdup(buf);
	if (g_path_is_absolute(path))
		return g_strdup(path);

	g_autofree gchar *cwd = g_get_current_dir();
	return g_build_filename(cwd, path, NULL);
}

gboolean
teco_file_is_visible(const gchar *path)
{
	g_autofree gchar *basename = g_path_get_basename(path);
	return *basename != '.';
}

#else /* !G_OS_UNIX */

#if GLIB_CHECK_VERSION(2,58,0)

/*
 * FIXME: This should perhaps be preferred on any platform.
 * But it will complicate preprocessing.
 */
gchar *
teco_file_get_absolute_path(const gchar *path)
{
	return g_canonicalize_filename(path, NULL);
}

#else /* !GLIB_CHECK_VERSION(2,58,0) */

/*
 * This will never canonicalize relative paths.
 * I.e. the absolute path will often contain
 * relative components, even if `path` exists.
 * The only exception would be a simple filename
 * not containing any "..".
 */
gchar *
teco_file_get_absolute_path(const gchar *path)
{
	if (!path)
		return NULL;
	if (g_path_is_absolute(path))
		return g_strdup(path);

	g_autofree gchar *cwd = g_get_current_dir();
	return g_build_filename(cwd, path, NULL);
}

#endif /* !GLIB_CHECK_VERSION(2,58,0) */

/*
 * There's no platform-independent way to determine if a file
 * is visible/hidden, so we just assume that all files are
 * visible.
 */
gboolean
teco_file_is_visible(const gchar *path)
{
	return TRUE;
}

#endif /* !G_OS_UNIX */

#endif /* !G_OS_WIN32 */

#ifdef G_OS_WIN32

gchar *
teco_file_get_program_path(void)
{
	TCHAR buf[MAX_PATH];
	if (!GetModuleFileNameW(NULL, buf, G_N_ELEMENTS(buf)))
		return g_get_current_dir();
	g_autofree gchar *exe = g_utf16_to_utf8(buf, -1, NULL, NULL, NULL);
	return g_path_get_dirname(exe);
}

#elif defined(__linux__)

gchar *
teco_file_get_program_path(void)
{
	gchar buf[PATH_MAX];
	ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf)-1);
	if (G_UNLIKELY(len < 0))
		/* almost certainly wrong */
		return g_get_current_dir();
	buf[len] = '\0';

	return g_path_get_dirname(buf);
}

#elif defined(G_OS_UNIX)

/*
 * At least works on FreeBSD, even though it also has
 * sysctl(KERN_PROC_PATHNAME).
 * We assume it works on all other UNIXes as well.
 */
gchar *
teco_file_get_program_path(void)
{
	Dl_info info;
	return dladdr(teco_file_get_program_path, &info)
		? g_path_get_dirname(info.dli_fname) : g_get_current_dir();
}

#else /* !G_OS_WIN32 && !__linux__ && !G_OS_UNIX */

/*
 * This is almost guaranteed to be wrong,
 * meaning that SciTECO cannot be made relocatable on these platforms.
 * It may be worth evaluating argv[0] on these platforms.
 */
gchar *
teco_file_get_program_path(void)
{
	return g_get_current_dir();
}

#endif

#ifdef G_OS_WIN32

/*
 * Definitions from the DDK's ntifs.h.
 */
#define FileCaseSensitiveInformation 71

static gboolean
teco_file_is_case_sensitive(const gchar *path)
{
	g_autofree gunichar2 *path_utf16 = g_utf8_to_utf16(path, -1, NULL, NULL, NULL);
	HANDLE hnd = CreateFileW(path_utf16, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, NULL,
	                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (hnd == INVALID_HANDLE_VALUE)
		return FALSE;

	/*
	 * NOTE: This requires Windows 10, version 1803 or later.
	 * FIXME: But even then, this is relying on undocumented behavior!
	 * If unavailable we just assume the platform-default case-insensitivity.
	 */
	FILE_CASE_SENSITIVE_INFORMATION info = {0};
	GetFileInformationByHandleEx(hnd, FileCaseSensitiveInformation, &info, sizeof(info));
	CloseHandle(hnd);
	return info.Flags & FILE_CS_FLAG_CASE_SENSITIVE_DIR;
}

#elif defined(G_OS_UNIX) && defined(_PC_CASE_SENSITIVE)

/*
 * This is supported at least on Mac OS.
 *
 * NOTE: If the selector is not supported, -1 is returned and we also assume case-sensitivity.
 */
static inline gboolean
teco_file_is_case_sensitive(const gchar *path)
{
	return pathconf(path, _PC_CASE_SENSITIVE);
}

#else /* !G_OS_WIN32 && (!G_OS_UNIX || !_PC_CASE_SENSITIVE) */

/*
 * FIXME: The only way to query this on Linux and FreeBSD would be to
 * hardcode "case-insensitive" file systems.
 */
static inline gboolean
teco_file_is_case_sensitive(const gchar *path)
{
	return TRUE;
}

#endif

/**
 * Get the datadir.
 *
 * By default it is hardcoded to an absolute path at
 * build time.
 * However, you can also build relocateable binaries
 * where the datadir is relative to the program's executable.
 *
 * @note Beginning with glib v2.58, we could directly use
 * g_canonicalize_filename().
 */
gchar *
teco_file_get_datadir(void)
{
	if (g_path_is_absolute(SCITECODATADIR))
		return g_strdup(SCITECODATADIR);

	/* relocateable binary - datadir is relative to binary */
	g_autofree gchar *program_path = teco_file_get_program_path();
	g_autofree gchar *datadir = g_build_filename(program_path, SCITECODATADIR, NULL);
	return teco_file_get_absolute_path(datadir);
}

/**
 * Perform tilde expansion on a file name or path.
 *
 * This supports only strings with a "~" prefix.
 * A user name after "~" is not supported.
 * The $HOME environment variable/register is used to retrieve
 * the current user's home directory.
 */
gchar *
teco_file_expand_path(const gchar *path)
{
	if (!path)
		return g_strdup("");

	if (path[0] != '~' || (path[1] && !G_IS_DIR_SEPARATOR(path[1])))
		return g_strdup(path);

	/*
	 * $HOME should not have a trailing directory separator since
	 * it is canonicalized to an absolute path at startup,
	 * but this ensures that a proper path is constructed even if
	 * it does (e.g. $HOME is changed later on).
	 *
	 * FIXME: In the future, it might be possible to remove the entire register.
	 */
	teco_qreg_t *qreg = teco_qreg_table_find(&teco_qreg_table_globals, "$HOME", 5);
	g_assert(qreg != NULL);

	/*
	 * Getting the string should not possible to fail.
	 * The $HOME register should not contain any null-bytes on startup,
	 * but it may have been changed later on.
	 */
	g_auto(teco_string_t) home = {NULL, 0};
	if (!qreg->vtable->get_string(qreg, &home.data, &home.len, NULL, NULL) ||
	    teco_string_contains(&home, '\0'))
		return g_strdup(path);
	g_assert(home.data != NULL);

	return g_build_filename(home.data, path+1, NULL);
}

/**
 * Auto-complete a filename/directory.
 *
 * @param filename The filename to auto-complete or NULL.
 * @param file_test Restrict completion to files matching the test.
 *                  If G_FILE_TEST_EXISTS, both files and directories are completed.
 *                  If G_FILE_TEST_IS_DIR, only directories will be completed.
 * @param insert String to initialize with the autocompletion.
 * @return TRUE if the completion was unambiguous (e.g. command can be terminated).
 */
gboolean
teco_file_auto_complete(const gchar *filename, GFileTest file_test, teco_string_t *insert)
{
	memset(insert, 0, sizeof(*insert));

	g_autofree gchar *filename_expanded = teco_file_expand_path(filename);
	gsize filename_len = strlen(filename_expanded);

	/*
	 * Derive base and directory names.
	 * We do not use g_path_get_basename() or g_path_get_dirname()
	 * since we need strict suffixes and prefixes of filename
	 * in order to construct paths of entries in dirname
	 * that are suitable for auto completion.
	 */
	gsize dirname_len = teco_file_get_dirname_len(filename_expanded);
	g_autofree gchar *dirname = g_strndup(filename_expanded, dirname_len);
	gchar *basename = filename_expanded + dirname_len;
	gsize basename_len = strlen(basename);

	g_autoptr(GDir) dir = g_dir_open(dirname_len ? dirname : ".", 0, NULL);
	if (!dir)
		return FALSE;

	/* Whether the directory has case-sensitive entries */
	gboolean case_sensitive = teco_file_is_case_sensitive(dirname_len ? dirname : ".");
	teco_string_diff_t string_diff = case_sensitive ? teco_string_diff : teco_string_casediff;

	/*
	 * On Windows, both forward and backslash
	 * directory separators are allowed in directory
	 * names passed to glib.
	 * To imitate glib's behaviour, we use
	 * the last valid directory separator in `filename_expanded`
	 * to generate new separators.
	 * This also allows forward-slash auto-completion
	 * on Windows.
	 */
	const gchar *dir_sep = dirname_len ? dirname + dirname_len - 1
	                                   : G_DIR_SEPARATOR_S;

	GSList *files = NULL;
	guint files_len = 0;
	gsize prefix_len = 0;

	teco_string_t cur_basename;
	while ((cur_basename.data = (gchar *)g_dir_read_name(dir))) {
		cur_basename.len = strlen(cur_basename.data);

		if (string_diff(&cur_basename, basename, basename_len) != basename_len)
			/* basename is not a prefix of cur_basename */
			continue;

		/*
		 * NOTE: `dirname` contains any directory separator, so strcat() works here.
		 * Reserving one byte at the end of the filename ensures we can easily
		 * append the directory separator without reallocations.
		 */
		gchar *cur_filename = g_malloc(strlen(dirname)+cur_basename.len+2);
		strcat(strcpy(cur_filename, dirname), cur_basename.data);

		/*
		 * NOTE: This avoids g_file_test() for G_FILE_TEST_EXISTS
		 * since the file we process here should always exist.
		 */
		if ((!*basename && !teco_file_is_visible(cur_filename)) ||
		    (file_test != G_FILE_TEST_EXISTS &&
		     !g_file_test(cur_filename, file_test))) {
			g_free(cur_filename);
			continue;
		}

		if (file_test == G_FILE_TEST_IS_DIR ||
		    g_file_test(cur_filename, G_FILE_TEST_IS_DIR))
			strcat(cur_filename, dir_sep);

		files = g_slist_prepend(files, cur_filename);

		if (g_slist_next(files)) {
			teco_string_t other_file;
			other_file.data = (gchar *)g_slist_next(files)->data + filename_len;
			other_file.len = strlen(other_file.data);

			gsize len = string_diff(&other_file, cur_filename + filename_len,
			                        strlen(cur_filename) - filename_len);
			if (len < prefix_len)
				prefix_len = len;
		} else {
			prefix_len = strlen(cur_filename + filename_len);
		}

		files_len++;
	}

	if (prefix_len > 0) {
		teco_string_init(insert, (gchar *)files->data + filename_len, prefix_len);
	} else if (files_len > 1) {
		files = g_slist_sort(files, (GCompareFunc)g_strcmp0);

		for (GSList *file = files; file; file = g_slist_next(file)) {
			teco_popup_entry_type_t type = TECO_POPUP_DIRECTORY;
			gboolean is_buffer = FALSE;

			if (!teco_file_is_dir((gchar *)file->data)) {
				type = TECO_POPUP_FILE;
				/* FIXME: inefficient */
				is_buffer = teco_ring_find((gchar *)file->data) != NULL;
			}

			teco_interface_popup_add(type, (gchar *)file->data,
			                         strlen((gchar *)file->data), is_buffer);
		}

		teco_interface_popup_show(filename ? strlen(filename) : 0);
	}

	/*
	 * FIXME: If we are completing only directories,
	 * we can theoretically insert the completed character
	 * after directories without subdirectories.
	 */
	gboolean unambiguous = files_len == 1 && !teco_file_is_dir((gchar *)files->data);
	g_slist_free_full(files, g_free);
	return unambiguous;
}
