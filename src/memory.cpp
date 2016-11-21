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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif

#include <glib.h>

#include "sciteco.h"
#include "memory.h"
#include "error.h"
#include "undo.h"

#ifdef HAVE_WINDOWS_H
/* here it shouldn't cause conflicts with other headers */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#endif

namespace SciTECO {

MemoryLimit memlimit;

#ifdef HAVE_MALLINFO

gsize
MemoryLimit::get_usage(void)
{
	struct mallinfo info = mallinfo();

	/*
	 * NOTE: `uordblks` is an int and thus prone
	 * to wrap-around issues.
	 *
	 * Unfortunately, the only other machine readable
	 * alternative is malloc_info() which prints
	 * into a FILE * stream [sic!] and is unspeakably
	 * slow even if writing to an unbuffered fmemopen()ed
	 * stream.
	 */
	return info.uordblks;
}

#elif defined(G_OS_WIN32)

gsize
MemoryLimit::get_usage(void)
{
	PROCESS_MEMORY_COUNTERS info;

	/*
	 * This __should__ not fail since the current process has
	 * PROCESS_ALL_ACCESS, but who knows...
	 * Since memory limiting cannot be turned off when this
	 * happens, we can just as well terminate abnormally.
	 */
	if (G_UNLIKELY(!GetProcessMemoryInfo(GetCurrentProcess(),
	                                     &info, sizeof(info)))) {
		gchar *msg = g_win32_error_message(GetLastError());
		g_error("Cannot get memory usage: %s", msg);
		/* shouldn't be reached */
		g_free(msg);
		return 0;
	}

	return info.WorkingSetSize;
}

#else

#define USE_MEMORY_COUNTING

static gsize memory_usage = 0;

gsize
MemoryLimit::get_usage(void)
{
	return memory_usage;
}

#endif /* !HAVE_MALLINFO && !G_OS_WIN32 */

void
MemoryLimit::set_limit(gsize new_limit)
{
	gsize memory_usage = get_usage();

	if (G_UNLIKELY(new_limit && memory_usage > new_limit)) {
		gchar *usage_str = g_format_size(memory_usage);
		gchar *limit_str = g_format_size(new_limit);

		Error err("Cannot set undo memory limit (%s): "
		          "Current usage too large (%s).",
		          limit_str, usage_str);

		g_free(limit_str);
		g_free(usage_str);
		throw err;
	}

	undo.push_var(limit) = new_limit;
}

void
MemoryLimit::check(void)
{
	if (G_UNLIKELY(limit && get_usage() > limit)) {
		gchar *limit_str = g_format_size(limit);

		Error err("Memory limit (%s) exceeded. See <EJ> command.",
		          limit_str);

		g_free(limit_str);
		throw err;
	}
}

void *
Object::operator new(size_t size) noexcept
{
#ifdef USE_MEMORY_COUNTING
	memory_usage += size;
#endif

	/*
	 * Since we've got the sized-delete operator
	 * below, we could allocate via g_slice.
	 *
	 * Using g_slice however would render malloc_trim()
	 * ineffective. Also, it has been shown to be
	 * unnecessary on Linux/glibc.
	 * Glib is guaranteed to use the system malloc(),
	 * so g_malloc() cooperates with malloc_trim().
	 *
	 * On Windows (even Windows 2000), the slice allocator
	 * did not show any significant performance boost
	 * either. Also, since g_slice never seems to return
	 * memory to the OS and we cannot force it to do so,
	 * it will not cooperate with the Windows-specific
	 * memory measurement and it is hard to recover
	 * from memory limit exhaustions.
	 */
	return g_malloc(size);
}

void
Object::operator delete(void *ptr, size_t size) noexcept
{
	g_free(ptr);

#ifdef USE_MEMORY_COUNTING
	memory_usage -= size;
#endif
}

} /* namespace SciTECO */
