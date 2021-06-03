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

#define USE_DL_PREFIX /* for dlmalloc */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif
#ifdef HAVE_MALLOC_NP_H
#include <malloc_np.h>
#endif

#ifdef HAVE_WINDOWS_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#endif

/*
 * For task_info() on OS X.
 */
#ifdef HAVE_MACH_MACH_H
#include <mach/mach.h>
#endif
#ifdef HAVE_MACH_MESSAGE_H
#include <mach/message.h>
#endif
#ifdef HAVE_MACH_KERN_RETURN_H
#include <mach/kern_return.h>
#endif
#ifdef HAVE_MACH_TASK_INFO_H
#include <mach/task_info.h>
#endif

/*
 * For sysctl() on FreeBSD.
 */
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_SYSCTL_H
#include <sys/sysctl.h>
#endif

/*
 * For sysconf() on Linux.
 */
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif

#include <glib.h>

/*
 * For open() (currently only on Linux).
 */
#ifdef G_OS_UNIX
#include <sys/stat.h>
#include <fcntl.h>
#endif

#include "sciteco.h"
#include "error.h"
#include "undo.h"
#include "memory.h"

/**
 * @file
 * Memory measurement and limiting.
 *
 * A discussion of memory measurement techniques on Linux
 * and UNIXoid operating systems is in order, since this
 * problem turned out to be rather tricky.
 *
 * @par Size of the program break
 * There is also the old-school technique of calculating the size
 * of the program break, ie. the effective size of the DATA segment.
 * This works under the assumption that all allocations are
 * performed by extending the program break, as is __traditionally__
 * done by malloc() and friends.
 *
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
 * - However, this might be a suitable backend on old UNIX-platforms
 *   or a as a fallback for teco_memory_get_usage().
 *
 * @par Resource limits
 * UNIX has resource limits, which could be used to enforce
 * the memory limit, but in case they are hit, malloc()
 * will return NULL, so g_malloc() would abort().
 * Wrapping malloc() to work around that has the same
 * problems described below.
 *
 * @par Hooking malloc()
 * malloc_usable_size() could be used to count the memory
 * consumption by updating a counter after every malloc(),
 * realloc() and free().
 * malloc_usable_size() is libc-specific, but available at least in
 * glibc and jemalloc (FreeBSD). Windows (MSVCRT) has `_msize()`.
 * This would require overwriting or hooking all calls to
 * malloc() and friends, though.
 * For all other platforms, we'd have to rely on writing the
 * heap object size into every heap object, thus wasting
 * one word per heap object.
 *
 * - glibc has malloc hooks, but they are non-portable and
 *   deprecated.
 * - It is possible to effectively wrap malloc() by overriding
 *   the libc's implementation, which will even work when
 *   statically linking in libc since malloc() is usually
 *   declared `weak`.
 *   This however does probably not work on all platforms and
 *   means you need to know the original function (pointers).
 *   It should work sufficiently when linking everything statically.
 * - glibc exports symbols for the original malloc() implementation
 *   like `__libc_malloc()` that could be used for wrapping.
 *   This is undocumented and libc-specific, though.
 * - The GNU ld --wrap option allows us to intercept calls,
 *   but obviously won't work for shared libraries.
 * - The portable dlsym() could be used to look up the original
 *   library symbol, but it may and does call malloc functions,
 *   eg. calloc() on glibc.
 *   Some people work around this using bootstrap makeshift allocators
 *   used only during dlsym().
 *   __In other words, there is no way to portably and reliably
 *   wrap malloc() and friends when linking dynamically.__
 * - Another difficulty is that, when free() is overridden, every
 *   function that can __independently__ allocate memory that
 *   can be passed to free() must also be overridden.
 *   This is impossible to know without making assumptions about the
 *   malloc implementation used.
 *   Otherwise the measurement is not precise and there can even
 *   be underruns. Thus we'd have to guard against underruns.
 * - Unfortunately, it is undefined whether the "usable" size of
 *   a heap object can change unwittingly, ie. not by malloc() or
 *   realloc() on that same heap object, but for instance after a
 *   neighbouring heap object is freed.
 *   If this can happen, free() on that heap object might subtract
 *   more than was initially added for this heap object, resulting
 *   in measurement underruns.
 * - malloc() and friends are MT-safe, so any replacement function
 *   would have to be MT-safe as well to avoid memory corruption.
 *
 * Memory counting using malloc_usable_size() in overwritten/wrapped
 * malloc()/realloc()/free() calls has thus been deemed impractical.
 *
 * Overriding could only work if we store the allocated size
 * at the beginning of each heap object and would link in an external
 * malloc() implementation, so that the symbol names are known.
 *
 * Unfortunately, overwriting libc functions is also non-portable,
 * so replacing the libc malloc with an external allocator is tricky.
 * On Linux (and hopefully other UNIXes), you can simply link
 * in the malloc replacement statically which will even let the
 * dynamic linker pick the new implementation.
 * On Windows however, we would apparently need incredibly hacky code
 * to patch the symbol tables
 * (see https://github.com/ned14/nedmalloc/blob/master/winpatcher.c).
 * Alternatively, everything __including__ MSVCRT needs to be linked
 * in statically. This is not supported by MinGW and would have certain
 * disadvantages even if it worked.
 *
 * @par malloc() introspection
 * glibc and some other platforms have mallinfo().
 * But at least on glibc it can get unbearably slow on programs
 * with a lot of (virtual/resident) memory.
 * Besides, mallinfo's API is broken on 64-bit systems, effectively
 * limiting the enforcable memory limit to 4GB.
 * Other glibc-specific introspection functions like malloc_info()
 * can be even slower because of the syscalls required.
 *
 * - FreeBSD/jemalloc has mallctl("stats.allocated") which even when
 *   optimized is significantly slower than the current implementation
 *   but generally acceptable.
 * - dlmalloc has malloc_footprint() which is very fast.
 *   It was therefore considered to simply import dlmalloc as the default
 *   allocator on (almost) all platforms.
 *   Despite problems overwriting malloc() globally on some platforms,
 *   this turned out to be impractical since malloc_footprint() includes
 *   only the mmapped memory and memory is not always unmapped even when
 *   calling malloc_trim(), so we couldn't recover after hitting
 *   the memory limit.
 * - rpmalloc has a cheap rpmalloc_global_statistics() but enabling it
 *   comes with a memory overhead.
 * - There seems to be no other malloc() replacement with a constant-time
 *   function returning the footprint.
 *
 * @par Instrumenting all of SciTECO's and C++ allocations.
 * If we don't want to count each and every allocation in the system,
 * we could also use custom allocators/deallocators together with
 * malloc_usable_size().
 * For many objects, the size will also be known at free() time, so
 * malloc_usable_size() can be avoided.
 *
 * - To track Scintilla's memory usage, custom C++ allocators/deallocators
 *   can be defined.
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
 * - This approach has the same disadvantages as wrapping malloc() because
 *   of the unreliability of malloc_usable_size().
 *   Furthermore, all allocations by glib (eg. g_strdup()) will be missed.
 *
 * @par Directly measuring the resident memory size
 * It is of course possible to query the program's RSS via OS APIs.
 * This has long been avoided because it is naturally platform-dependant and
 * some of the APIs have proven to be too slow for frequent polling.
 *
 * - Windows has GetProcessMemoryInfo() which is quite slow.
 *   When polled on a separate thread, the slow down is very acceptable.
 * - OS X has task_info().
 *   __Its performance is still untested!__
 * - FreeBSD has sysctl().
 *   __Its performance is still untested!__
 * - Linux has no APIs but /proc/self/statm.
 *   Reading it is naturally very slow, but at least of constant time.
 *   When polled on a separate thread, the slow down is very acceptable.
 *   Also, use of malloc_trim() after hitting the memory limit is crucial
 *   since the RSS will otherwise not decrease.
 * - Haiku has no usable constant-time API.
 *
 * @par Conclusion
 * Every approach sucks and no platform supports everything.
 * We therefore now opted for a combined strategy:
 * Most platforms will by default try to replace malloc() with dlmalloc.
 * The dlmalloc functions are wrapped and the memory usage is counted via
 * malloc_usable_size() which in the case of dlmalloc should never change
 * for one heap object unless we realloc() it.
 * This should be fastest, the most precise and there is a guaranteed
 * malloc_trim().
 * Malloc overriding can be disabled at compile time to aid in memory
 * debugging.
 * On Windows, we never even try to link in dlmalloc.
 * If disabled, we try to directly measure memory consumption using
 * OS APIs.
 * Polling of the RSS takes place in a dedicated thread that is started
 * on demand and paused whenever the main thread is idle (eg. waits for
 * user input), so we don't waste cycles.
 */

/**
 * Current memory usage.
 * Access must be synchronized using atomic operations.
 */
static gint teco_memory_usage = 0;

/*
 * NOTE: This implementation based on malloc_usable_size() might
 * also work with other malloc libraries, given that they provide
 * a malloc_usable_size() which does not change for a heap object
 * (unless it is reallocated of course).
 */
#ifdef REPLACE_MALLOC

void * __attribute__((used))
malloc(size_t size)
{
	void *ptr = dlmalloc(size);
	if (G_LIKELY(ptr != NULL))
		g_atomic_int_add(&teco_memory_usage, dlmalloc_usable_size(ptr));
	return ptr;
}

void __attribute__((used))
free(void *ptr)
{
	if (!ptr)
		return;
	g_atomic_int_add(&teco_memory_usage, -dlmalloc_usable_size(ptr));
	dlfree(ptr);
}

void * __attribute__((used))
calloc(size_t nmemb, size_t size)
{
	void *ptr = dlcalloc(nmemb, size);
	if (G_LIKELY(ptr != NULL))
		g_atomic_int_add(&teco_memory_usage, dlmalloc_usable_size(ptr));
	return ptr;
}

void * __attribute__((used))
realloc(void *ptr, size_t size)
{
	if (ptr)
		g_atomic_int_add(&teco_memory_usage, -dlmalloc_usable_size(ptr));
	ptr = dlrealloc(ptr, size);
	if (G_LIKELY(ptr != NULL))
		g_atomic_int_add(&teco_memory_usage, dlmalloc_usable_size(ptr));
	return ptr;
}

void * __attribute__((used))
memalign(size_t alignment, size_t size)
{
	void *ptr = dlmemalign(alignment, size);
	if (G_LIKELY(ptr != NULL))
		g_atomic_int_add(&teco_memory_usage, dlmalloc_usable_size(ptr));
	return ptr;
}

void * __attribute__((used))
aligned_alloc(size_t alignment, size_t size)
{
	return memalign(alignment, size);
}

int __attribute__((used))
posix_memalign(void **memptr, size_t alignment, size_t size)
{
	int ret = dlposix_memalign(memptr, alignment, size);
	if (G_LIKELY(!ret))
		g_atomic_int_add(&teco_memory_usage, dlmalloc_usable_size(*memptr));
	return ret;
}

void * __attribute__((used))
valloc(size_t size)
{
	void *ptr = dlvalloc(size);
	if (G_LIKELY(ptr != NULL))
		g_atomic_int_add(&teco_memory_usage, dlmalloc_usable_size(ptr));
	return ptr;
}

/*
 * The glibc manual claims we have to replace this function
 * but we'd need sysconf(_SC_PAGESIZE) to implement it.
 */
void * __attribute__((used))
pvalloc(size_t size)
{
	g_assert_not_reached();
	return NULL;
}

size_t __attribute__((used))
malloc_usable_size(void *ptr)
{
	return dlmalloc_usable_size(ptr);
}

int __attribute__((used))
malloc_trim(size_t pad)
{
	return dlmalloc_trim(pad);
}

/*
 * FIXME: Which platforms might need malloc_trim() to
 * recover from hitting the memory limit?
 * In other words, which platform's teco_memory_get_usage()
 * might return a large value even if most memory has already
 * been deallocated?
 */
#elif defined(G_OS_WIN32)

/*
 * On Windows, we never link in dlmalloc.
 *
 * NOTE: At least on Windows 2000, we run twice as fast than
 * when polling from a dedicated thread.
 */
static gsize
teco_memory_get_usage(void)
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
		g_autofree gchar *msg = g_win32_error_message(GetLastError());
		g_error("Cannot get memory usage: %s", msg);
		return 0;
	}

	return info.WorkingSetSize;
}

#define NEED_POLL_THREAD

#elif defined(HAVE_TASK_INFO)

/*
 * Practically only for Mac OS X.
 *
 * FIXME: Benchmark whether polling in a thread really
 * improves performances as much as on Linux.
 * Is this even critical or can we link in dlmalloc?
 */
static gsize
teco_memory_get_usage(void)
{
	struct mach_task_basic_info info;
	mach_msg_type_number_t info_count = MACH_TASK_BASIC_INFO_COUNT;

	if (G_UNLIKELY(task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
	                         (task_info_t)&info, &info_count) != KERN_SUCCESS))
		return 0; // FIXME

	return info.resident_size;
}

#define NEED_POLL_THREAD

#elif defined(G_OS_UNIX) && defined(HAVE_SYSCTL)

/*
 * Practically only for FreeBSD.
 *
 * FIXME: Is this even critical or can we link in dlmalloc?
 */
static gsize
teco_memory_get_usage(void)
{
	struct kinfo_proc procstk;
	size_t len = sizeof(procstk);
	int pidinfo[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};

	sysctl(pidinfo, G_N_ELEMENTS(pidinfo), &procstk, &len, NULL, 0);

	return procstk.ki_rssize; // FIXME: Which unit?
}

#define NEED_POLL_THREAD

#elif defined(G_OS_UNIX) && defined(HAVE_SYSCONF) && defined(HAVE_PROCFS)

#ifndef HAVE_MALLOC_TRIM
#warning malloc_trim() missing - Might not recover from hitting the memory limit!
#endif

/*
 * Mainly for Linux, but there might be other UNIXoids supporting procfs.
 * This would be ridiculously slow if polled from the main thread.
 *
 * Since Linux supports dlmalloc(), this will usually not be required
 * unless you disable it explicitly.
 *
 * NOTE: This is conciously avoiding glib and stdio APIs since we run in
 * a very tight loop and should avoid any unnecessary allocations which could
 * significantly slow down the main thread.
 */
static gsize
teco_memory_get_usage(void)
{
	static long page_size = 0;

	if (G_UNLIKELY(!page_size))
		page_size = sysconf(_SC_PAGESIZE);

	int fd = open("/proc/self/statm", O_RDONLY);
	if (fd < 0)
		/* procfs might not be mounted */
		return 0;

	gchar buf[256];
	ssize_t len = read(fd, buf, sizeof(buf)-1);
	close(fd);
	if (G_UNLIKELY(len < 0))
		return 0;
	buf[len] = '\0';

	gsize memory_usage = 0;
	sscanf(buf, "%*u %" G_GSIZE_FORMAT, &memory_usage);

	return memory_usage * page_size;
}

#define NEED_POLL_THREAD

#else

/*
 * We've got neither dlmalloc, nor any particular OS backend.
 */
#warning dlmalloc is disabled and there is no memory counting backend - memory limiting will be unavailable!

#endif

#ifdef NEED_POLL_THREAD

static GThread *teco_memory_thread = NULL;

static enum {
	TECO_MEMORY_STATE_ON,
	TECO_MEMORY_STATE_OFF,
	TECO_MEMORY_STATE_SHUTDOWN
} teco_memory_state = TECO_MEMORY_STATE_ON;

static GMutex teco_memory_mutex;
static GCond teco_memory_cond;

/*
 * FIXME: What if we activated the thread only whenever the
 * usage is queried in the main thread?
 * This would automatically "clock" the threaded polling at the same rate
 * as the main thread is polling.
 * On the downside, the value of teco_memory_usage would be more outdated,
 * so a memory overrun would be detected with even more delay.
 */
static gpointer
teco_memory_poll_thread_cb(gpointer data)
{
	g_mutex_lock(&teco_memory_mutex);

	for (;;) {
		while (teco_memory_state == TECO_MEMORY_STATE_ON) {
			g_mutex_unlock(&teco_memory_mutex);
			/*
			 * NOTE: teco_memory_mutex is not used for teco_memory_usage
			 * since it is locked most of the time which would extremely slow
			 * down the main thread.
			 */
			g_atomic_int_set(&teco_memory_usage, teco_memory_get_usage());
			g_thread_yield();
			g_mutex_lock(&teco_memory_mutex);
		}
		if (G_UNLIKELY(teco_memory_state == TECO_MEMORY_STATE_SHUTDOWN))
			break;

		g_cond_wait(&teco_memory_cond, &teco_memory_mutex);
		/* teco_memory_mutex is locked */
	}

	g_mutex_unlock(&teco_memory_mutex);
	return NULL;
}

void __attribute__((constructor))
teco_memory_start_limiting(void)
{
	if (!teco_memory_limit)
		return;

	/*
	 * FIXME: Setting a low thread priority would certainly help.
	 * This would be less important for platforms like Linux where
	 * we usually don't need a polling thread at all.
	 */
	if (G_UNLIKELY(!teco_memory_thread))
		teco_memory_thread = g_thread_new(NULL, teco_memory_poll_thread_cb, NULL);

	g_mutex_lock(&teco_memory_mutex);
	teco_memory_state = TECO_MEMORY_STATE_ON;
	g_cond_signal(&teco_memory_cond);
	g_mutex_unlock(&teco_memory_mutex);
}

void
teco_memory_stop_limiting(void)
{
	g_mutex_lock(&teco_memory_mutex);
	teco_memory_state = TECO_MEMORY_STATE_OFF;
	g_mutex_unlock(&teco_memory_mutex);
}

#ifndef NDEBUG
static void __attribute__((destructor))
teco_memory_cleanup(void)
{
	if (!teco_memory_thread)
		return;

	g_mutex_lock(&teco_memory_mutex);
	teco_memory_state = TECO_MEMORY_STATE_SHUTDOWN;
	g_cond_signal(&teco_memory_cond);
	g_mutex_unlock(&teco_memory_mutex);

	g_thread_join(teco_memory_thread);
}
#endif

#else /* !NEED_POLL_THREAD */

void teco_memory_start_limiting(void) {}
void teco_memory_stop_limiting(void) {}

#endif

/**
 * Memory limit in bytes (500mb by default, assuming SI units).
 * 0 means no limiting.
 */
gsize teco_memory_limit = 500*1000*1000;

gboolean
teco_memory_set_limit(gsize new_limit, GError **error)
{
	gsize memory_usage = g_atomic_int_get(&teco_memory_usage);

	if (G_UNLIKELY(new_limit && memory_usage > new_limit)) {
		g_autofree gchar *usage_str = g_format_size(memory_usage);
		g_autofree gchar *limit_str = g_format_size(new_limit);

		g_set_error(error, TECO_ERROR, TECO_ERROR_FAILED,
		            "Cannot set undo memory limit (%s): "
		            "Current usage too large (%s).",
		            limit_str, usage_str);
		return FALSE;
	}

	teco_undo_gsize(teco_memory_limit) = new_limit;

	if (teco_memory_limit)
		teco_memory_start_limiting();
	else
		teco_memory_stop_limiting();

	return TRUE;
}

/**
 * Check whether the memory limit is exceeded or would be
 * exceeded by an allocation.
 *
 * @param request Size of the requested allocation or 0 if
 *                you want to check the current memory usage.
 */
gboolean
teco_memory_check(gsize request, GError **error)
{
	gsize memory_usage = g_atomic_int_get(&teco_memory_usage) + request;

	if (G_UNLIKELY(teco_memory_limit && memory_usage > teco_memory_limit)) {
		g_autofree gchar *limit_str = g_format_size(memory_usage);

		g_set_error(error, TECO_ERROR, TECO_ERROR_MEMLIMIT,
		            "Memory limit (%s) exceeded. See <EJ> command.",
		            limit_str);
		return FALSE;
	}

	return TRUE;
}
