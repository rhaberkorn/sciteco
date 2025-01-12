/*
 * Copyright (C) 2012-2025 Robin Haberkorn
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
#include "qreg.h"
#include "undo.h"

/**
 * Defines a function undo__insert_val__ARRAY() to insert a value into
 * a fixed GArray.
 *
 * @note
 * This optimizes undo token memory consumption under the assumption
 * that ARRAY is a global object that does not have to be stored in
 * the undo tokens.
 * Otherwise, you could simply undo__g_array_insert_val(...).
 *
 * @fixme
 * If we only ever use INDEX == ARRAY->len, we might simplify this
 * to undo__append_val__ARRAY().
 */
#define TECO_DEFINE_ARRAY_UNDO_INSERT_VAL(ARRAY, TYPE) \
	static inline void \
	insert_val__##ARRAY(guint index, TYPE value) \
	{ \
		g_array_insert_val(ARRAY, index, value); \
	} \
	TECO_DEFINE_UNDO_CALL(insert_val__##ARRAY, guint, TYPE)

/**
 * Defines a function undo__remove_index__ARRAY() to remove a value from
 * a fixed GArray.
 *
 * @note
 * See TECO_DEFINE_ARRAY_UNDO_INSERT_VAL().
 * undo__g_array_remove_index(...) would also be possible.
 *
 * @fixme
 * If we only ever use INDEX == ARRAY->len-1, we might simplify this
 * to undo__pop__ARRAY().
 */
#define TECO_DEFINE_ARRAY_UNDO_REMOVE_INDEX(ARRAY) \
	static inline void \
	remove_index__##ARRAY(guint index) \
	{ \
		g_array_remove_index(ARRAY, index); \
	} \
	TECO_DEFINE_UNDO_CALL(remove_index__##ARRAY, guint)

/**
 * Operator type.
 * The enumeration value divided by 16 represents
 * its precedence (small values mean low precedence).
 * In other words, the value's lower nibble is
 * reserved for enumerating operators of the
 * same precedence.
 */
typedef enum {
	/*
	 * Pseudo operators
	 */
	TECO_OP_NIL	= 0x00,
	TECO_OP_NEW,
	TECO_OP_BRACE,
	TECO_OP_NUMBER,
	/*
	 * Real operators
	 */
	TECO_OP_POW	= 0x60,	// ^*
	TECO_OP_MOD	= 0x50,	// ^/
	TECO_OP_DIV,		// /
	TECO_OP_MUL,		// *
	TECO_OP_SUB	= 0x40,	// -
	TECO_OP_ADD,		// +
	TECO_OP_AND	= 0x30,	// &
	TECO_OP_XOR	= 0x20,	// ^#
	TECO_OP_OR	= 0x10	// #
} teco_operator_t;

extern gint teco_num_sign;

static inline void
teco_set_num_sign(gint sign)
{
	teco_undo_gint(teco_num_sign) = sign;
}

void teco_expressions_push_int(teco_int_t number);

/** Push characters of a C-string. */
static inline void
teco_expressions_push_str(const gchar *str)
{
	while (*str)
		teco_expressions_push_int(*str++);
}

teco_int_t teco_expressions_peek_num(guint index);
teco_int_t teco_expressions_pop_num(guint index);
gboolean teco_expressions_pop_num_calc(teco_int_t *ret, teco_int_t imply, GError **error);

void teco_expressions_add_digit(gunichar digit, teco_qreg_t *radix);

void teco_expressions_push_op(teco_operator_t op);
gboolean teco_expressions_push_calc(teco_operator_t op, GError **error);

/*
 * FIXME: Does not work for TECO_OP_* constants as they are treated like int.
 */
#define teco_expressions_push(X) \
	(_Generic((X), default      : teco_expressions_push_int, \
	               char *       : teco_expressions_push_str, \
	               const char * : teco_expressions_push_str)(X))

teco_operator_t teco_expressions_peek_op(guint index);
teco_operator_t teco_expressions_pop_op(guint index);

gboolean teco_expressions_eval(gboolean pop_brace, GError **error);

guint teco_expressions_args(void);

gint teco_expressions_first_op(void);

gboolean teco_expressions_discard_args(GError **error);

extern guint teco_brace_level;

void teco_expressions_brace_open(void);
gboolean teco_expressions_brace_return(guint keep_braces, guint args, GError **error);
gboolean teco_expressions_brace_close(GError **error);

void teco_expressions_clear(void);

/** Maximum size required to format a number if radix == 2 */
#define TECO_EXPRESSIONS_FORMAT_LEN \
        (1 + sizeof(teco_int_t)*8 + 1)

gchar *teco_expressions_format(gchar *buffer, teco_int_t number, teco_qreg_t *radix);
