/*
 * Copyright (C) 2012-2022 Robin Haberkorn
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

#include <glib.h>

#include "sciteco.h"

extern gboolean teco_undo_enabled;

/**
 * A callback to be invoked when an undo token gets executed or cleaned up.
 *
 * @note Unless you want to cast user_data in every callback implementation,
 * you may want to cast your callback type instead to teco_undo_action_t.
 * Casting to functions of different signature is theoretically undefined behavior,
 * but works on all major platforms including Emscripten, as long as they differ only
 * in pointer types.
 *
 * @param user_data
 * The data allocated by teco_undo_push_size() (usually a context structure).
 * You are supposed to free any external resources (heap pointers etc.) referenced
 * from it till the end of the callback.
 * @param run
 * Whether the operation should actually be performed instead of merely freeing
 * the associated memory.
 */
typedef void (*teco_undo_action_t)(gpointer user_data, gboolean run);

gpointer teco_undo_push_size(teco_undo_action_t action_cb, gsize size)
         G_GNUC_ALLOC_SIZE(2);

#define teco_undo_push(NAME) \
        ((NAME##_t *)teco_undo_push_size((teco_undo_action_t)NAME##_action, \
	                                 sizeof(NAME##_t)))

/**
 * @defgroup undo_objects Undo objects
 *
 * @note
 * The general meta programming approach here is similar to C++ explicit template
 * instantiation.
 * A macro is expanded for every object type into some compilation unit and a declaration
 * into the corresponding header.
 * The object's type is mangled into the generated "push"-function's name.
 * In case of scalars, C11 Generics and some macro magic is then used to hide the
 * type names and for "reference" style passing.
 *
 * Explicit instantiation could be theoretically avoided using GCC compound expressions
 * and nested functions. However, GCC will inevitably generate trampolines which
 * are unportable and induce a runtime penalty.
 * Furthermore, nested functions are not supported by Clang, where the Blocks extension
 * would have to be used instead.
 * Another alternative for implicit instantiation would be preprocessing of all source
 * files with some custom M4 macros.
 */
/*
 * FIXME: Due to the requirements on the variable, we could be tempted to inline
 * references to it directly into the action()-function, saving the `ptr`
 * in the undo token. This is however often practically not possible.
 * We could however add a variant for true global variables.
 *
 * FIXME: Sometimes, the push-function is used only in a single compilation unit,
 * so it should be declared `static` or `static inline`.
 * Is it worth complicating our APIs in order to support that?
 *
 * FIXME: Perhaps better split this into TECO_DEFINE_UNDO_OBJECT() and TECO_DEFINE_UNDO_OBJECT_OWN()
 */
#define __TECO_DEFINE_UNDO_OBJECT(NAME, TYPE, COPY, DELETE, DELETE_IF_DISABLED, DELETE_ON_RUN) \
	typedef struct { \
		TYPE *ptr; \
		TYPE value; \
	} teco_undo_object_##NAME##_t; \
	\
	static void \
	teco_undo_object_##NAME##_action(teco_undo_object_##NAME##_t *ctx, gboolean run) \
	{ \
		if (run) { \
			DELETE_ON_RUN(*ctx->ptr); \
			*ctx->ptr = ctx->value; \
		} else { \
			DELETE(ctx->value); \
		} \
	} \
	\
	/** @ingroup undo_objects */ \
	TYPE * \
	teco_undo_object_##NAME##_push(TYPE *ptr) \
	{ \
		teco_undo_object_##NAME##_t *ctx = teco_undo_push(teco_undo_object_##NAME); \
		if (ctx) { \
			ctx->ptr = ptr; \
			ctx->value = COPY(*ptr); \
		} else { \
			DELETE_IF_DISABLED(*ptr); \
		} \
		return ptr; \
	}

/**
 * Defines an undo token push function that when executed restores
 * the value/state of a variable of TYPE to the value it had when this
 * was called.
 *
 * This can be used to undo changes to arbitrary variables, either
 * requiring explicit memory handling or to scalars.
 *
 * The lifetime of the variable must be global - a pointer to it must be valid
 * until the undo token could be executed.
 * This will usually exclude stack-allocated variables or objects.
 *
 * @param NAME C identifier used for name mangling.
 * @param TYPE Type of variable to restore.
 * @param COPY A global function/expression to execute in order to copy VAR.
 *             If left empty, this is an identity operation and ownership
 *             of the variable is passed to the undo token.
 * @param DELETE A global function/expression to execute in order to destruct
 *               objects of TYPE. Leave empty if destruction is not necessary.
 *
 * @ingroup undo_objects
 */
#define TECO_DEFINE_UNDO_OBJECT(NAME, TYPE, COPY, DELETE) \
	__TECO_DEFINE_UNDO_OBJECT(NAME, TYPE, COPY, DELETE, /* don't delete if disabled */, DELETE)
/**
 * @fixme _OWN variants will invalidate the variable pointer, so perhaps
 * it will be clearer to have _SET variants instead.
 *
 * @ingroup undo_objects
 */
#define TECO_DEFINE_UNDO_OBJECT_OWN(NAME, TYPE, DELETE) \
	__TECO_DEFINE_UNDO_OBJECT(NAME, TYPE, /* pass ownership */, DELETE, DELETE, /* don't delete if run */)

/** @ingroup undo_objects */
#define TECO_DECLARE_UNDO_OBJECT(NAME, TYPE) \
	TYPE *teco_undo_object_##NAME##_push(TYPE *ptr)

/** @ingroup undo_objects */
#define TECO_DEFINE_UNDO_SCALAR(TYPE) \
	TECO_DEFINE_UNDO_OBJECT_OWN(TYPE, TYPE, /* don't delete */)

/** @ingroup undo_objects */
#define TECO_DECLARE_UNDO_SCALAR(TYPE) \
	TECO_DECLARE_UNDO_OBJECT(TYPE, TYPE)

/*
 * FIXME: We had to add -Wno-unused-value to surpress warnings.
 * Perhaps it's clearer to sacrifice the lvalue feature.
 *
 * TODO: Check whether generating an additional check on teco_undo_enabled here
 * significantly improves batch-mode performance.
 */

TECO_DECLARE_UNDO_SCALAR(gchar);
#define teco_undo_gchar(VAR) (*teco_undo_object_gchar_push(&(VAR)))

TECO_DECLARE_UNDO_SCALAR(gint);
#define teco_undo_gint(VAR) (*teco_undo_object_gint_push(&(VAR)))

TECO_DECLARE_UNDO_SCALAR(guint);
#define teco_undo_guint(VAR) (*teco_undo_object_guint_push(&(VAR)))

TECO_DECLARE_UNDO_SCALAR(gsize);
#define teco_undo_gsize(VAR) (*teco_undo_object_gsize_push(&(VAR)))

TECO_DECLARE_UNDO_SCALAR(teco_int_t);
#define teco_undo_int(VAR) (*teco_undo_object_teco_int_t_push(&(VAR)))

TECO_DECLARE_UNDO_SCALAR(gboolean);
#define teco_undo_gboolean(VAR) (*teco_undo_object_gboolean_push(&(VAR)))

TECO_DECLARE_UNDO_SCALAR(gconstpointer);
#define teco_undo_ptr(VAR) \
	(*(typeof(VAR) *)teco_undo_object_gconstpointer_push((gconstpointer *)&(VAR)))

#define __TECO_GEN_STRUCT(ID, X)	X arg_##ID;
//#define __TECO_GEN_ARG(ID, X)		X arg_##ID,
//#define __TECO_GEN_ARG_LAST(ID, X)	X arg_##ID
#define __TECO_GEN_CALL(ID, X)		ctx->arg_##ID,
#define __TECO_GEN_CALL_LAST(ID, X)	ctx->arg_##ID
#define __TECO_GEN_INIT(ID, X)		ctx->arg_##ID = arg_##ID;

/**
 * @defgroup undo_calls Function calls on rubout.
 * @{
 */

/**
 * Create an undo token that calls FNC with arbitrary scalar parameters
 * (maximum 5, captured at the time of the call).
 * It defines a function undo__FNC() for actually creating the closure.
 *
 * All arguments must be constants or expressions evaluating to scalars though,
 * since no memory management (copying/freeing) is performed.
 *
 * Tipp: In order to save memory in the undo token structures, it is
 * often trivial to define a static inline function that calls FNC and binds
 * "constant" parameters.
 *
 * @param FNC Name of a global function or macro to execute.
 *            It must be a plain C identifier.
 * @param ... The parameter types of FNC (signature).
 *            Only the types without any variable names must be specified.
 *
 * @fixme Sometimes, the push-function is used only in a single compilation unit,
 * so it should be declared `static` or `static inline`.
 * Is it worth complicating our APIs in order to support that?
 */
#define TECO_DEFINE_UNDO_CALL(FNC, ...) \
	typedef struct { \
		TECO_FOR_EACH(__TECO_GEN_STRUCT, __TECO_GEN_STRUCT, ##__VA_ARGS__) \
	} teco_undo_call_##FNC##_t; \
	\
	static void \
	teco_undo_call_##FNC##_action(teco_undo_call_##FNC##_t *ctx, gboolean run) \
	{ \
		if (run) \
			FNC(TECO_FOR_EACH(__TECO_GEN_CALL, __TECO_GEN_CALL_LAST, ##__VA_ARGS__)); \
	} \
	\
	/** @ingroup undo_calls */ \
	void \
	undo__##FNC(TECO_FOR_EACH(__TECO_GEN_ARG, __TECO_GEN_ARG_LAST, ##__VA_ARGS__)) \
	{ \
		teco_undo_call_##FNC##_t *ctx = teco_undo_push(teco_undo_call_##FNC); \
		if (ctx) { \
			TECO_FOR_EACH(__TECO_GEN_INIT, __TECO_GEN_INIT, ##__VA_ARGS__) \
		} \
	}

/** @} */

void teco_undo_pop(gint pc);
void teco_undo_clear(void);
