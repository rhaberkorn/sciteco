/*
 * Copyright (C) 2012-2024 Robin Haberkorn
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
#pragma once

#include <stdio.h>
#include <signal.h>

#include <glib.h>

#include <Scintilla.h>

#if TECO_INTEGER == 32
typedef gint32 teco_int_t;
#define TECO_INT_FORMAT G_GINT32_FORMAT
#elif TECO_INTEGER == 64
typedef gint64 teco_int_t;
#define TECO_INT_FORMAT G_GINT64_FORMAT
#else
#error Invalid TECO integer storage size
#endif

/**
 * A TECO boolean - this differs from C booleans.
 * See teco_is_success()/teco_is_failure().
 */
typedef teco_int_t teco_bool_t;

#define TECO_SUCCESS ((teco_bool_t)-1)
#define TECO_FAILURE ((teco_bool_t)0)

static inline teco_bool_t
teco_bool(gboolean x)
{
	return x ? TECO_SUCCESS : TECO_FAILURE;
}

static inline gboolean
teco_is_success(teco_bool_t x)
{
	return x < 0;
}

static inline gboolean
teco_is_failure(teco_bool_t x)
{
	return x >= 0;
}

/**
 * Call function as destructor on debug builds.
 * This should be used only if the cleanup is optional.
 */
#ifdef NDEBUG
#define TECO_DEBUG_CLEANUP __attribute__((unused))
#else
#define TECO_DEBUG_CLEANUP __attribute__((destructor))
#endif

/** TRUE if C is a control character */
#define TECO_IS_CTL(C)		((gunichar)(C) < ' ')
/** ASCII character to echo control character C */
#define TECO_CTL_ECHO(C)	((C) | 0x40)
/**
 * Control character of ASCII C, i.e.
 * control character corresponding to CTRL+C keypress.
 */
#define TECO_CTL_KEY(C)		((C) & ~0x40)

/**
 * ED flags.
 * This is not a bitfield, since it is set from SciTECO.
 */
enum {
	TECO_ED_DEFAULT_ANSI	= (1 << 2),
	TECO_ED_AUTOCASEFOLD	= (1 << 3),
	TECO_ED_AUTOEOL		= (1 << 4),
	TECO_ED_HOOKS		= (1 << 5),
	TECO_ED_FNKEYS		= (1 << 6),
	TECO_ED_SHELLEMU	= (1 << 7),
	TECO_ED_XTERM_CLIPBOARD	= (1 << 8)
};

/* in main.c */
extern teco_int_t teco_ed;

static inline guint
teco_default_codepage(void)
{
	return teco_ed & TECO_ED_DEFAULT_ANSI ? SC_CHARSET_ANSI : SC_CP_UTF8;
}

/* in main.c */
extern volatile sig_atomic_t teco_interrupted;

/*
 * Allows automatic cleanup of FILE pointers.
 */
G_DEFINE_AUTOPTR_CLEANUP_FUNC(FILE, fclose);

/*
 * BEWARE DRAGONS!
 */
#define __TECO_FE_0(WHAT, WHAT_LAST)
#define __TECO_FE_1(WHAT, WHAT_LAST, X)      WHAT_LAST(1,X)
#define __TECO_FE_2(WHAT, WHAT_LAST, X, ...) WHAT(2,X)__TECO_FE_1(WHAT, WHAT_LAST, __VA_ARGS__)
#define __TECO_FE_3(WHAT, WHAT_LAST, X, ...) WHAT(3,X)__TECO_FE_2(WHAT, WHAT_LAST, __VA_ARGS__)
#define __TECO_FE_4(WHAT, WHAT_LAST, X, ...) WHAT(4,X)__TECO_FE_3(WHAT, WHAT_LAST, __VA_ARGS__)
#define __TECO_FE_5(WHAT, WHAT_LAST, X, ...) WHAT(5,X)__TECO_FE_4(WHAT, WHAT_LAST, __VA_ARGS__)
//... repeat as needed

#define __TECO_GET_MACRO(_0,_1,_2,_3,_4,_5,NAME,...) NAME

/**
 * Invoke macro `action(ID, ARG)` on every argument
 * and `action_last(ID, ARG)` on the very last one.
 * Currently works only for 5 arguments,
 * but if more are needed you can add __TECO_FE_X macros.
 */
#define TECO_FOR_EACH(action, action_last, ...) \
        __TECO_GET_MACRO(_0,##__VA_ARGS__,__TECO_FE_5,__TECO_FE_4,__TECO_FE_3, \
	                                  __TECO_FE_2,__TECO_FE_1,__TECO_FE_0)(action,action_last,##__VA_ARGS__)

#define __TECO_GEN_ARG(ID, X)			X arg_##ID,
#define __TECO_GEN_ARG_LAST(ID, X)		X arg_##ID
#define __TECO_VTABLE_GEN_CALL(ID, X)		arg_##ID,
#define __TECO_VTABLE_GEN_CALL_LAST(ID, X)	arg_##ID

#define TECO_DECLARE_VTABLE_METHOD(RET_TYPE, NS, NAME, OBJ_TYPE, ...) \
	static inline RET_TYPE \
	NS##_##NAME(OBJ_TYPE ctx, ##TECO_FOR_EACH(__TECO_GEN_ARG, __TECO_ARG_LAST, ##__VA_ARGS__)) \
	{ \
		return ctx->vtable->NAME(ctx, ##TECO_FOR_EACH(__TECO_VTABLE_GEN_CALL, __TECO_VTABLE_GEN_CALL_LAST, ##__VA_ARGS__)); \
	} \
	typedef RET_TYPE (*NS##_##NAME##_t)(OBJ_TYPE, ##__VA_ARGS__)
