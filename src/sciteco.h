/*
 * Copyright (C) 2012-2015 Robin Haberkorn
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

namespace SciTECO {

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
		ED_AUTOEOL	= (1 << 4),
		ED_HOOKS	= (1 << 5),
		ED_FNKEYS	= (1 << 6),
		ED_SHELLEMU	= (1 << 7)
	};

	extern tecoInt ed;
}

extern sig_atomic_t sigint_occurred;

/**
 * For sentinels: NULL might not be defined as a
 * pointer type (LLVM/CLang)
 */
#define NIL		((void *)0)

/** true if C is a control character */
#define IS_CTL(C)	((C) < ' ')
/** ASCII character to echo control character C */
#define CTL_ECHO(C)	((C) | 0x40)
/**
 * Control character of ASCII C, i.e.
 * control character corresponding to CTRL+<C> keypress.
 */
#define CTL_KEY(C)	((C) & ~0x40)
/**
 * Control character of the escape key.
 * Equivalent to CTL_KEY('[')
 */
#define CTL_KEY_ESC	27
/** String containing the escape character */
#define CTL_KEY_ESC_STR	"\x1B"

#define SUCCESS		(-1)
#define FAILURE		(0)
#define TECO_BOOL(X)	((X) ? SUCCESS : FAILURE)

#define IS_SUCCESS(X)	((X) < 0)
#define IS_FAILURE(X)	(!IS_SUCCESS(X))

/* in main.cpp */
void interrupt(void);

/* in main.cpp */
const gchar *get_eol_seq(gint eol_mode);

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

} /* namespace SciTECO */

#endif
