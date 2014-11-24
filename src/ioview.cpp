/*
 * Copyright (C) 2012-2014 Robin Haberkorn
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

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>

#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>

#include <Scintilla.h>

#include "sciteco.h"
#include "interface.h"
#include "undo.h"
#include "error.h"
#include "ioview.h"

#ifdef HAVE_WINDOWS_H
/* here it shouldn't cause conflicts with other headers */
#include <windows.h>

/* still need to clean up */
#undef interface
#endif

namespace SciTECO {

#ifdef G_OS_WIN32

typedef DWORD FileAttributes;
/* INVALID_FILE_ATTRIBUTES already defined */

static inline FileAttributes
get_file_attributes(const gchar *filename)
{
	return GetFileAttributes((LPCTSTR)filename);
}

static inline void
set_file_attributes(const gchar *filename, FileAttributes attrs)
{
	SetFileAttributes((LPCTSTR)filename, attrs);
}

#else

typedef int FileAttributes;
#define INVALID_FILE_ATTRIBUTES (-1)

static inline FileAttributes
get_file_attributes(const gchar *filename)
{
	struct stat buf;

	return g_stat(filename, &buf) ? INVALID_FILE_ATTRIBUTES : buf.st_mode;
}

static inline void
set_file_attributes(const gchar *filename, FileAttributes attrs)
{
	g_chmod(filename, attrs);
}

#endif /* !G_OS_WIN32 */

/*
 * The following simple implementation of file reading is actually the
 * most efficient and useful in the common case of editing small files,
 * since
 * a) it works with minimal number of syscalls and
 * b) small files cause little temporary memory overhead.
 * Reading large files however could be very inefficient since the file
 * must first be read into memory and then copied in-memory. Also it could
 * result in thrashing.
 * Alternatively we could iteratively read into a smaller buffer trading
 * in speed against (temporary) memory consumption.
 * The best way to do it could be memory mapping the file as we could
 * let Scintilla copy from the file's virtual memory directly.
 * Unfortunately since every page of the mapped file is
 * only touched once by Scintilla TLB caching is useless and the TLB is
 * effectively thrashed with entries of the mapped file.
 * This results in the doubling of page faults and weighs out the other
 * advantages of memory mapping (has been benchmarked).
 *
 * So in the future, the following approach could be implemented:
 * 1.) On Unix/Posix, mmap() one page at a time, hopefully preventing
 *     TLB thrashing.
 * 2.) On other platforms read into and copy from a statically sized buffer
 *     (perhaps page-sized)
 */
void
IOView::load(const gchar *filename)
{
	gchar *contents;
	gsize size;

	GError *gerror = NULL;

	if (!g_file_get_contents(filename, &contents, &size, &gerror))
		throw GlibError(gerror);

	ssm(SCI_BEGINUNDOACTION);
	ssm(SCI_CLEARALL);
	ssm(SCI_APPENDTEXT, size, (sptr_t)contents);
	ssm(SCI_ENDUNDOACTION);

	g_free(contents);
}

#if 0

/*
 * TODO: on UNIX it may be better to open() the current file, unlink() it
 * and keep the file descriptor in the UndoToken.
 * When the operation is undone, the file descriptor's contents are written to
 * the file (which should be efficient enough because it is written to the same
 * filesystem). This way we could avoid messing around with save point files.
 */

#else

static gint savepoint_id = 0;

class UndoTokenRestoreSavePoint : public UndoToken {
	gchar	*savepoint;
	gchar	*filename;

#ifdef G_OS_WIN32
	FileAttributes orig_attrs;
#endif

public:
	UndoTokenRestoreSavePoint(gchar *_savepoint, const gchar *_filename)
				 : savepoint(_savepoint), filename(g_strdup(_filename))
	{
#ifdef G_OS_WIN32
		orig_attrs = get_file_attributes(filename);
		if (orig_attrs != INVALID_FILE_ATTRIBUTES)
			set_file_attributes(savepoint,
			                    orig_attrs | FILE_ATTRIBUTE_HIDDEN);
#endif
	}

	~UndoTokenRestoreSavePoint()
	{
		if (savepoint) {
			g_unlink(savepoint);
			g_free(savepoint);
		}
		g_free(filename);

		savepoint_id--;
	}

	void
	run(void)
	{
		if (!g_rename(savepoint, filename)) {
			g_free(savepoint);
			savepoint = NULL;
#ifdef G_OS_WIN32
			if (orig_attrs != INVALID_FILE_ATTRIBUTES)
				set_file_attributes(filename,
						    orig_attrs);
#endif
		} else {
			interface.msg(InterfaceCurrent::MSG_WARNING,
				      "Unable to restore save point file \"%s\"",
				      savepoint);
		}
	}
};

static void
make_savepoint(const gchar *filename)
{
	gchar *dirname, *basename, *savepoint;
	gchar savepoint_basename[FILENAME_MAX];

	basename = g_path_get_basename(filename);
	g_snprintf(savepoint_basename, sizeof(savepoint_basename),
		   ".teco-%d-%s", savepoint_id, basename);
	g_free(basename);
	dirname = g_path_get_dirname(filename);
	savepoint = g_build_filename(dirname, savepoint_basename, NIL);
	g_free(dirname);

	if (g_rename(filename, savepoint)) {
		interface.msg(InterfaceCurrent::MSG_WARNING,
			      "Unable to create save point file \"%s\"",
			      savepoint);
		g_free(savepoint);
		return;
	}
	savepoint_id++;

	/* NOTE: passes ownership of savepoint string to undo token */
	undo.push(new UndoTokenRestoreSavePoint(savepoint, filename));
}

#endif /* !G_OS_UNIX */

void
IOView::save(const gchar *filename)
{
	const void *buffer;
	sptr_t gap;
	size_t size;
	FILE *file;

#ifdef G_OS_UNIX
	GStatBuf file_stat;
	file_stat.st_uid = -1;
	file_stat.st_gid = -1;
#endif
	FileAttributes attributes = INVALID_FILE_ATTRIBUTES;

	if (undo.enabled) {
		if (g_file_test(filename, G_FILE_TEST_IS_REGULAR)) {
#ifdef G_OS_UNIX
			g_stat(filename, &file_stat);
#endif
			attributes = get_file_attributes(filename);
			make_savepoint(filename);
		} else {
			undo.push(new UndoTokenRemoveFile(filename));
		}
	}

	/* leaves access mode intact if file still exists */
	file = g_fopen(filename, "w");
	if (!file)
		/* hopefully, errno is also always set on Windows */
		throw Error("Error opening file \"%s\" for writing: %s",
		            filename, strerror(errno));

	/* write part of buffer before gap */
	gap = ssm(SCI_GETGAPPOSITION);
	if (gap > 0) {
		buffer = (const void *)ssm(SCI_GETRANGEPOINTER,
		                           0, gap);
		if (!fwrite(buffer, (size_t)gap, 1, file)) {
			fclose(file);
			throw Error("Error writing file \"%s\"",
			            filename);
		}
	}

	/* write part of buffer after gap */
	size = ssm(SCI_GETLENGTH) - gap;
	if (size > 0) {
		buffer = (const void *)ssm(SCI_GETRANGEPOINTER,
		                           gap, size);
		if (!fwrite(buffer, size, 1, file)) {
			fclose(file);
			throw Error("Error writing file \"%s\"",
			            filename);
		}
	}

	/* if file existed but has been renamed, restore attributes */
	if (attributes != INVALID_FILE_ATTRIBUTES)
		set_file_attributes(filename, attributes);
#ifdef G_OS_UNIX
	/*
	 * only a good try to inherit owner since process user must have
	 * CHOWN capability traditionally reserved to root only.
	 * That's why we don't handle the return value and are spammed
	 * with unused-result warnings by GCC. There is NO sane way to avoid
	 * this warning except, adding -Wno-unused-result which disabled all
	 * such warnings.
	 */
	fchown(fileno(file), file_stat.st_uid, file_stat.st_gid);
#endif

	fclose(file);
}

/*
 * Auxiliary functions
 */
#ifdef G_OS_UNIX

gchar *
get_absolute_path(const gchar *path)
{
	gchar buf[PATH_MAX];
	gchar *resolved;

	if (!path)
		return NULL;

	if (!realpath(path, buf)) {
		if (g_path_is_absolute(path)) {
			resolved = g_strdup(path);
		} else {
			gchar *cwd = g_get_current_dir();
			resolved = g_build_filename(cwd, path, NIL);
			g_free(cwd);
		}
	} else {
		resolved = g_strdup(buf);
	}

	return resolved;
}

#elif defined(G_OS_WIN32)

gchar *
get_absolute_path(const gchar *path)
{
	TCHAR buf[MAX_PATH];
	gchar *resolved = NULL;

	if (path && GetFullPathName(path, sizeof(buf), buf, NULL))
		resolved = g_strdup(buf);

	return resolved;
}

#else

/*
 * FIXME: I doubt that works on any platform...
 */
gchar *
get_absolute_path(const gchar *path)
{
	return path ? g_file_read_link(path, NULL) : NULL;
}

#endif /* !G_OS_UNIX && !G_OS_WIN32 */

} /* namespace SciTECO */
