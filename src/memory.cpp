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

#include <stdlib.h>
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif
#ifdef HAVE_MALLOC_NP_H
#include <malloc_np.h>
#endif
#ifdef HAVE_DLSYM
#include <dlfcn.h>
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

#if defined(HAVE_DLSYM) && defined(HAVE_MALLOC_USABLE_SIZE)
/*
 * This should work on most UNIXoid systems.
 *
 * We "hook" into the malloc-functions and count the
 * "usable" size of each memory block (which may be
 * more than what has been requested).
 * This effectively counts all allocations by malloc(),
 * g_malloc() and any C++ new() everywhere, has minimal overhead and
 * is much faster than the Linux-specific mallinfo().
 */

static gsize memory_usage = 0;

gsize
MemoryLimit::get_usage(void)
{
	return memory_usage;
}

extern "C" {

void *
malloc(size_t size)
{
	typedef void *(*malloc_cb)(size_t);
	static malloc_cb libc_malloc = NULL;
	void *ret;

	if (G_UNLIKELY(!libc_malloc))
		libc_malloc = (malloc_cb)dlsym(RTLD_NEXT, "malloc");

	ret = libc_malloc(size);
	if (G_LIKELY(ret))
		memory_usage += malloc_usable_size(ret);

	return ret;
}

void *
realloc(void *ptr, size_t size)
{
	typedef void *(*realloc_cb)(void *, size_t);
	static realloc_cb libc_realloc = NULL;

	if (G_UNLIKELY(!libc_realloc))
		libc_realloc = (realloc_cb)dlsym(RTLD_NEXT, "realloc");

	if (ptr)
		memory_usage -= malloc_usable_size(ptr);
	ptr = libc_realloc(ptr, size);
	if (G_LIKELY(ptr))
		memory_usage += malloc_usable_size(ptr);

	return ptr;
}

void
free(void *ptr)
{
	typedef void (*free_cb)(void *);
	static free_cb libc_free = NULL;

	if (G_UNLIKELY(!libc_free))
		libc_free = (free_cb)dlsym(RTLD_NEXT, "free");

	if (ptr)
		memory_usage -= malloc_usable_size(ptr);
	libc_free(ptr);
}

} /* extern "C" */

#elif defined(G_OS_WIN32)
/*
 * Uses the Windows-specific GetProcessMemoryInfo(),
 * so the entire process heap is measured.
 *
 * FIXME: Unfortunately, this is much slower than the portable
 * fallback implementation.
 * We should try and benchmark a similar approach to the
 * UNIX implementation above using MSVCRT-specific APIs (_minfo()).
 */

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
/*
 * Portable fallback-implementation relying on C++11 sized allocators.
 *
 * Unfortunately, this will only measure the heap used by C++ objects
 * in SciTECO's sources; not even Scintilla, nor all g_malloc() calls.
 */

#define MEMORY_USAGE_FALLBACK

static gsize memory_usage = 0;

gsize
MemoryLimit::get_usage(void)
{
	return memory_usage;
}

#endif /* (!HAVE_DLSYM || !HAVE_MALLOC_USABLE_SIZE) && !G_OS_WIN32 */

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
#ifdef MEMORY_USAGE_FALLBACK
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

#ifdef MEMORY_USAGE_FALLBACK
	memory_usage -= size;
#endif
}

} /* namespace SciTECO */
