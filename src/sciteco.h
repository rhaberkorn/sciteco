/*
 * Copyright (C) 2012 Robin Haberkorn
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

#include <glib.h>

#include "interface.h"

namespace Flags {
	enum {
		ED_HOOKS = (1 << 5)
	};

	extern gint64 ed;
}

extern gchar *cmdline;
extern bool quit_requested;

void cmdline_keypress(gchar key);

#define IS_CTL(C)	((C) < ' ')
#define CTL_ECHO(C)	((C) | 0x40)
#define CTL_KEY(C)	((C) & ~0x40)

typedef gint64 tecoBool;

#define SUCCESS		(-1)
#define FAILURE		(0)
#define TECO_BOOL(X)	((X) ? SUCCESS : FAILURE)

#define IS_SUCCESS(X)	((X) < 0)
#define IS_FAILURE(X)	(!IS_SUCCESS(X))

#define CHR2STR(X)	((gchar []){X, '\0'})

namespace String {

static inline void
append(gchar *&str1, const gchar *str2)
{
	/* FIXME: optimize */
	gchar *new_str = g_strconcat(str1 ? : "", str2, NULL);
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
