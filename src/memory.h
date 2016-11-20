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

#ifndef __MEMORY_H
#define __MEMORY_H

#include <glib.h>

/**
 * Default memory limit (500mb, assuming SI units).
 */
#define MEMORY_LIMIT_DEFAULT (500*1000*1000)

namespace SciTECO {

/**
 * Common base class for all objects in SciTECO.
 * This is currently only used to provide custom new/delete
 * replacements in order to support unified allocation via
 * Glib (g_malloc and g_slice) and as a memory usage
 * counting fallback.
 *
 * This approach has certain drawbacks, e.g. you cannot
 * derive from Object privately; nor is it possible to
 * influence allocations in other libraries or even of
 * scalars (e.g. new char[5]).
 *
 * C++14 (supported by GCC >= 5) has global sized delete
 * replacements which would be effective in the entire application
 * but we're still using the base-class approach since
 * we must support the older compilers anyway.
 */
class Object {
public:
	static void *operator new(size_t size) noexcept;
	static inline void *
	operator new[](size_t size) noexcept
	{
		return operator new(size);
	}
	static inline void *
	operator new(size_t size, void *ptr) noexcept
	{
		return ptr;
	}

	static void operator delete(void *ptr, size_t size) noexcept;
	static inline void
	operator delete[](void *ptr, size_t size) noexcept
	{
		operator delete(ptr, size);
	}
};

extern class MemoryLimit : public Object {
public:
	/**
	 * Undo stack memory limit in bytes.
	 * 0 means no limiting.
	 */
	gsize limit;

	MemoryLimit() : limit(MEMORY_LIMIT_DEFAULT) {}

	gsize get_usage(void);

	void set_limit(gsize new_limit = 0);

	void check(void);
} memlimit;

} /* namespace SciTECO */

#endif
