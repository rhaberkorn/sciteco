/*
 * Copyright (C) 2012-2013 Robin Haberkorn
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

#ifndef __SCITECO_H
#define __SCITECO_H

#include <signal.h>

#include <glib.h>

#include "interface.h"

#if TECO_INTEGER == 32
typedef gint32 tecoInt;
#define TECO_INTEGER_FORMAT G_GINT32_FORMAT
#elif TECO_INTEGER == 64
typedef gint64 tecoInt;
#define TECO_INTEGER_FORMAT G_GINT64_FORMAT
#else
#error Invalid TECO integer storage size
#endif
typedef tecoInt tecoBool;

namespace Flags {
	enum {
		ED_HOOKS	= (1 << 5),
		ED_FNKEYS	= (1 << 6)
	};

	extern tecoInt ed;
}

extern sig_atomic_t sigint_occurred;

/*
 * for sentinels: NULL might not be defined as a
 * pointer type (LLVM/CLang)
 */
#define NIL		((void *)0)

#define IS_CTL(C)	((C) < ' ')
#define CTL_ECHO(C)	((C) | 0x40)
#define CTL_KEY(C)	((C) & ~0x40)

#define SUCCESS		(-1)
#define FAILURE		(0)
#define TECO_BOOL(X)	((X) ? SUCCESS : FAILURE)

#define IS_SUCCESS(X)	((X) < 0)
#define IS_FAILURE(X)	(!IS_SUCCESS(X))

/*
 * NOTE: compound literals are temporaries beginning with
 * g++ 4.7
 */
#define CHR2STR(X, ...) \
	({gchar str[] = {(X), ##__VA_ARGS__, '\0'}; str;})

namespace String {

static inline void
append(gchar *&str1, const gchar *str2)
{
	/* FIXME: optimize */
	gchar *new_str = g_strconcat(str1 ? : "", str2, NIL);
	g_free(str1);
	str1 = new_str;
}

static inline void
append(gchar *&str, gchar chr)
{
	append(str, CHR2STR(chr));
}

} /* namespace String */

namespace Validate {

static inline bool
pos(gint n)
{
	return n >= 0 && n <= interface.ssm(SCI_GETLENGTH);
}

static inline bool
line(gint n)
{
	return n >= 0 && n < interface.ssm(SCI_GETLINECOUNT);
}

} /* namespace Validate */

#endif
