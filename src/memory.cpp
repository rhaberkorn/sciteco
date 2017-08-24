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

/* for malloc_usable_size() */
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif
#ifdef HAVE_MALLOC_NP_H
#include <malloc_np.h>
#endif

#include <new>

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

/*
 * Define this to prefix each heap object allocated
 * by the custom allocators with a magic value.
 * This helps to detect non-matching calls to the
 * overridden new/delete operators which can cause
 * underruns of the memory counter.
 */
//#define DEBUG_MAGIC ((guintptr)0xDEAD15DE5E1BEAF0)

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
 * - glibc and some other platforms have mallinfo().
 *   But at least on glibc it can get unbearably slow on programs
 *   with a lot of (virtual/resident) memory.
 *   Besides, mallinfo's API is broken on 64-bit systems, effectively
 *   limiting the enforcable memory limit to 4GB.
 *   Other glibc-specific introspection functions like malloc_info()
 *   can be even slower because of the syscalls required.
 * - Linux has /proc/self/stat and /proc/self/statm but polling them
 *   is very inefficient.
 * - FreeBSD/jemalloc has mallctl("stats.allocated") which even when
 *   optimized is significantly slower than the fallback but generally
 *   acceptable.
 * - On all other platforms we (have to) rely on the fallback
 *   implementation based on C++ allocators/deallocators.
 *   They have been improved significantly to count as much memory
 *   as possible, even using libc-specific APIs like malloc_usable_size().
 *   Since this has been proven to work sufficiently well even on FreeBSD,
 *   there is no longer any UNIX-specific implementation.
 *   Even the malloc_usable_size() workaround for old or non-GNU
 *   compilers is still faster than mallctl() on FreeBSD.
 *   This might need to change in the future.
 * - Beginning with C++14 (or earlier with -fsized-deallocation),
 *   it is possible to globally replace sized allocation/deallocation
 *   functions, which could be used to avoid the malloc_usable_size()
 *   workaround. Unfortunately, this may not be used for arrays,
 *   since the compiler may have to call non-sized variants if the
 *   original allocation size is unknown - and there is no way to detect
 *   that when the new[] call is made.
 *   What's worse is that at least G++ STL is broken seriously and
 *   some versions will call the non-sized delete() even when sized-deallocation
 *   is available. Again, this cannot be detected at new() time.
 *   Therefore, I had to remove the sized-deallocation based
 *   optimization.
 */

#ifdef G_OS_WIN32
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
 * We might also just use the fallback implementation with some
 * additional support for _msize().
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
 * Unfortunately, in the worst case, this will only measure the heap used
 * by C++ objects in SciTECO's sources; not even Scintilla, nor all
 * g_malloc() calls.
 * Usually, we will be able to use global non-sized deallocators with
 * libc-specific support to get more accurate results, though.
 */

#define MEMORY_USAGE_FALLBACK

/**
 * Current memory usage in bytes.
 *
 * @bug This only works in single-threaded applications.
 *      Should SciTECO or Scintilla ever use multiple threads,
 *      it will be necessary to use atomic operations.
 */
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

/*
 * The object-specific sized deallocators allow memory
 * counting portably, even in strict C++11 mode.
 * Once we depend on C++14, they and the entire `Object`
 * class hack may be avoided.
 * But see above - due to broken STLs, this may not actually
 * be safe!
 */

void *
Object::operator new(size_t size) noexcept
{
#ifdef MEMORY_USAGE_FALLBACK
	memory_usage += size;
#endif

#ifdef DEBUG_MAGIC
	guintptr *ptr = (guintptr *)g_malloc(sizeof(guintptr) + size);
	*ptr = DEBUG_MAGIC;
	return ptr + 1;
#else
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
#endif
}

void
Object::operator delete(void *ptr, size_t size) noexcept
{
#ifdef DEBUG_MAGIC
	if (ptr) {
		ptr = (guintptr *)ptr - 1;
		g_assert(*(guintptr *)ptr == DEBUG_MAGIC);
	}
#endif

	g_free(ptr);

#ifdef MEMORY_USAGE_FALLBACK
	memory_usage -= size;
#endif
}

} /* namespace SciTECO */

/*
 * In strict C++11, we can still use global non-sized
 * deallocators.
 *
 * On their own, they bring little benefit, but with
 * some libc-specific functionality, they can be used
 * to improve the fallback memory measurements to include
 * all allocations (including Scintilla).
 * This comes with a moderate runtime penalty.
 *
 * Unfortunately, even in C++14, defining replacement
 * sized deallocators may be very dangerous, so this
 * seems to be as best as we can get (see above).
 */

void *
operator new(size_t size)
{
	void *ptr = g_malloc(size);

#if defined(MEMORY_USAGE_FALLBACK) && defined(HAVE_MALLOC_USABLE_SIZE)
	/* NOTE: g_malloc() should always use the system malloc(). */
	SciTECO::memory_usage += malloc_usable_size(ptr);
#endif

	return ptr;
}

void
operator delete(void *ptr) noexcept
{
#if defined(MEMORY_USAGE_FALLBACK) && defined(HAVE_MALLOC_USABLE_SIZE)
	if (ptr)
		SciTECO::memory_usage -= malloc_usable_size(ptr);
#endif
	g_free(ptr);
}
