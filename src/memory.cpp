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

#include <stdint.h>

#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif
#ifdef HAVE_MALLOC_NP_H
#include <malloc_np.h>
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

/*
 * A discussion of memory measurement techniques on Linux
 * and UNIXoid operating systems is in order, since this
 * problem turned out to be rather tricky.
 *
 * - UNIX has resource limits, which could be used to enforce
 *   the memory limit, but in case they are hit, malloc()
 *   will return NULL, so g_malloc() would abort().
 *   Wrapping malloc() to work around that has the same
 *   problems described below.
 * - glibc has malloc hooks, but they are non-portable and
 *   deprecated.
 * - It is possible to effectively wrap malloc() by overriding
 *   the libc's implementation, which will even work when
 *   statically linking in libc since malloc() is usually
 *   delcared `weak`.
 * - When wrapping malloc(), malloc_usable_size() could be
 *   used to count the memory consumption.
 *   This is libc-specific, but available at least in
 *   glibc and jemalloc (FreeBSD).
 * - glibc exports symbols for the original malloc() implementation
 *   like __libc_malloc() that could be used for wrapping.
 *   This is undocumented and libc-specific, though.
 * - The GNU ld --wrap option allows us to intercept calls,
 *   but obviously won't work for shared libraries.
 * - The portable dlsym() could be used to look up the original
 *   library symbol, but it may and does call malloc functions,
 *   eg. calloc() on glibc.
 *   In other words, there is no way to portably and reliably
 *   wrap malloc() and friends when linking dynamically.
 * - Another difficulty is that, when free() is overridden, every
 *   function that can __independently__ allocate memory that
 *   can be passed to free() must also be overridden.
 *   Otherwise the measurement is not precise and there can even
 *   be underruns. Thus we'd have to guard against underruns.
 * - malloc() and friends are MT-safe, so any replacement function
 *   would have to be MT-safe as well to avoid memory corruption.
 *   E.g. even in single-threaded builds, glib might use
 *   threads internally.
 * - There is also the old-school technique of calculating the size
 *   of the program break, ie. the effective size of the DATA segment.
 *   This works under the assumption that all allocations are
 *   performed by extending the program break, as is __traditionally__
 *   done by malloc() and friends.
 * - Unfortunately, modern malloc() implementations sometimes
 *   mmap() memory, especially for large allocations.
 *   SciTECO mostly allocates small chunks.
 *   Unfortunately, some malloc implementations like jemalloc
 *   only claim memory using mmap(), thus rendering sbrk(0)
 *   useless.
 * - Furthermore, some malloc-implementations like glibc will
 *   only shrink the program break when told so explicitly
 *   using malloc_trim(0).
 * - The sbrk(0) method thus depends on implementation details
 *   of the libc.
 * - For these reasons, we rather stick to non-portable,
 *   libc-specific, perhaps slow, but stable techniques to measure
 *   memory usage.
 *   Implementations for yet unsupported UNIXoid systems might
 *   still want to pick up any of the ideas above, if they can be
 *   proven to work well on those platforms.
 */

#ifdef HAVE_MALLINFO
/*
 * Linux/glibc-specific implementation.
 * Unfortunately, this slows things down when called frequently.
 */

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

#elif defined(HAVE_MALLCTLNAMETOMIB) && defined(HAVE_MALLCTLBYMIB)
/*
 * FreeBSD/jemalloc-specific implementation.
 * Unfortunately, this slows things down when called frequently.
 */

gsize
MemoryLimit::get_usage(void)
{
	static size_t epoch_mib[1] = {0};
	static size_t stats_allocated_mib[2] = {0};

	uint64_t epoch = 1;
	size_t stats_allocated;
	size_t stats_allocated_len = sizeof(stats_allocated);

	if (G_UNLIKELY(!epoch_mib[0])) {
		size_t len;
		int rc;

		len = G_N_ELEMENTS(epoch_mib);
		rc = mallctlnametomib("epoch", epoch_mib, &len);
		g_assert(rc == 0 && len == G_N_ELEMENTS(epoch_mib));

		len = G_N_ELEMENTS(stats_allocated_mib);
		rc = mallctlnametomib("stats.allocated",
		                      stats_allocated_mib, &len);
		g_assert(rc == 0 && len == G_N_ELEMENTS(stats_allocated_mib));
	}

	/* refresh statistics */
	mallctlbymib(epoch_mib, G_N_ELEMENTS(epoch_mib),
	             NULL, NULL, &epoch, sizeof(epoch));
	/* query the total number of allocated bytes */
	mallctlbymib(stats_allocated_mib, G_N_ELEMENTS(stats_allocated_mib),
	             &stats_allocated, &stats_allocated_len, NULL, 0);

	return stats_allocated;
}

#elif defined(G_OS_WIN32)
/*
 * Uses the Windows-specific GetProcessMemoryInfo(),
 * so the entire process heap is measured.
 *
 * FIXME: Unfortunately, this is much slower than the portable
 * fallback implementation.
 * It may be possible to overwrite malloc() and friends,
 * counting the chunks with the MSVCRT-specific _minfo().
 * Since we will always run against MSVCRT, the disadvantages
 * discussed above for the UNIX-case may not be important.
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

#endif /* MEMORY_USAGE_FALLBACK */

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
