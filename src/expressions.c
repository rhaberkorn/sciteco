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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib.h>

#include "sciteco.h"
#include "error.h"
#include "undo.h"
#include "qreg.h"
#include "expressions.h"

/*
 * Number and operator stacks are static, so
 * they can be passed to the undo token constructors.
 * This is OK since we're currently singleton.
 */
static GArray *teco_numbers;

TECO_DEFINE_ARRAY_UNDO_INSERT_VAL(teco_numbers, teco_int_t);
TECO_DEFINE_ARRAY_UNDO_REMOVE_INDEX(teco_numbers);

static GArray *teco_operators;

TECO_DEFINE_ARRAY_UNDO_INSERT_VAL(teco_operators, teco_operator_t);
TECO_DEFINE_ARRAY_UNDO_REMOVE_INDEX(teco_operators);

static gboolean teco_expressions_calc(GError **error);

static void __attribute__((constructor))
teco_expressions_init(void)
{
	teco_numbers = g_array_sized_new(FALSE, FALSE, sizeof(teco_int_t), 1024);
	teco_operators = g_array_sized_new(FALSE, FALSE, sizeof(teco_operator_t), 1024);
}

/** Get operator precedence */
static inline gint
teco_expressions_precedence(teco_operator_t op)
{
	return op >> 4;
}

gint teco_num_sign = 1;

void
teco_expressions_push_int(teco_int_t number)
{
	while (teco_operators->len > 0 && teco_expressions_peek_op(0) == TECO_OP_NEW)
		teco_expressions_pop_op(0);

	teco_expressions_push_op(TECO_OP_NUMBER);

	if (teco_num_sign < 0) {
		teco_set_num_sign(1);
		number *= -1;
	}

	g_array_append_val(teco_numbers, number);
	undo__remove_index__teco_numbers(teco_numbers->len-1);
}

/** Peek into the numbers stack */
teco_int_t
teco_expressions_peek_num(guint index)
{
	return g_array_index(teco_numbers, teco_int_t, teco_numbers->len - 1 - index);
}

/**
 * Pop a value from the number stack.
 *
 * This must only be called if you are sure that the number at the
 * given index exists and is an argument, i.e. only after calling
 * teco_expressions_eval() and teco_expressions_args().
 * If you are unsure or want to imply a value,
 * use teco_expressions_pop_num_calc() instead.
 */
teco_int_t
teco_expressions_pop_num(guint index)
{
	teco_int_t n = 0;
	G_GNUC_UNUSED teco_operator_t op = teco_expressions_pop_op(0);

	g_assert(op == TECO_OP_NUMBER);

	if (teco_numbers->len > 0) {
		n = teco_expressions_peek_num(index);
		undo__insert_val__teco_numbers(teco_numbers->len - 1 - index, n);
		g_array_remove_index(teco_numbers, teco_numbers->len - 1 - index);
	}

	return n;
}

/**
 * Pop an argument from the number stack.
 *
 * This resolves operations and allows implying values
 * if there isn't any argument on the stack.
 *
 * @param ret Where to store the number
 * @param imply The fallback value if there is no argument
 * @param error A GError
 * @return FALSE if an error occurred
 */
gboolean
teco_expressions_pop_num_calc(teco_int_t *ret, teco_int_t imply, GError **error)
{
	if (!teco_expressions_eval(FALSE, error))
		return FALSE;
	if (teco_num_sign < 0)
		teco_set_num_sign(1);

	teco_int_t v = teco_expressions_args() > 0 ? teco_expressions_pop_num(0) : imply;
	if (ret)
		*ret = v;
	return TRUE;
}

void
teco_expressions_add_digit(gunichar digit, teco_qreg_t *qreg)
{
	/*
	 * FIXME: We could just access qreg->integer here since
	 * we can assume that "^R" is a plain register.
	 */
	assert(qreg != NULL);
	teco_int_t radix = 10;
	qreg->vtable->get_integer(qreg, &radix, NULL);

	teco_int_t n = teco_expressions_args() > 0 ? teco_expressions_pop_num(0) : 0;

	/* use g_unichar_digit_value()? */
	teco_expressions_push(n*radix + (n < 0 ? -1 : 1)*((gint)digit - '0'));
}

void
teco_expressions_push_op(teco_operator_t op)
{
	g_array_append_val(teco_operators, op);
	undo__remove_index__teco_operators(teco_operators->len-1);
}

gboolean
teco_expressions_push_calc(teco_operator_t op, GError **error)
{
	for (;;) {
		gint first = teco_expressions_first_op();

		/* calculate if op has lower precedence than op on stack */
		if (first < 0 ||
		    teco_expressions_precedence(op) > teco_expressions_precedence(teco_expressions_peek_op(first)))
			break;

		if (!teco_expressions_calc(error))
			return FALSE;
	}

	teco_expressions_push_op(op);
	return TRUE;
}

teco_operator_t
teco_expressions_peek_op(guint index)
{
	return g_array_index(teco_operators, teco_operator_t, teco_operators->len - 1 - index);
}

teco_operator_t
teco_expressions_pop_op(guint index)
{
	teco_operator_t op = TECO_OP_NIL;

	if (teco_operators->len > 0) {
		op = teco_expressions_peek_op(index);
		undo__insert_val__teco_operators(teco_operators->len - 1 - index, op);
		g_array_remove_index(teco_operators, teco_operators->len - 1 - index);
	}

	return op;
}

static gboolean
teco_expressions_calc(GError **error)
{
	teco_int_t result;

	if (!teco_operators->len || teco_expressions_peek_op(0) != TECO_OP_NUMBER) {
		g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
		                    "Missing right operand");
		return FALSE;
	}
	teco_int_t vright = teco_expressions_pop_num(0);
	teco_operator_t op = teco_expressions_pop_op(0);
	if (!teco_operators->len || teco_expressions_peek_op(0) != TECO_OP_NUMBER) {
		g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
		                    "Missing left operand");
		return FALSE;
	}
	teco_int_t vleft = teco_expressions_pop_num(0);

	switch (op) {
	case TECO_OP_POW:
		if (!vright) {
			result = vleft < 0 ? -1 : 1;
			break;
		}
		if (vright < 0) {
			if (!vleft) {
				g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
				                    "Negative power of 0 is not defined");
				return FALSE;
			}
			result = ABS(vleft) == 1 ? vleft : 0;
			break;
		}
		result = 1;
		for (;;) {
			if (vright & 1)
				result *= vleft;
			vright >>= 1;
			if (!vright)
				break;
			vleft *= vleft;
		}
		break;
	case TECO_OP_MUL:
		result = vleft * vright;
		break;
	case TECO_OP_DIV:
		if (!vright) {
			g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
			                    "Division by zero");
			return FALSE;
		}
		result = vleft / vright;
		break;
	case TECO_OP_MOD:
		if (!vright) {
			g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
			                    "Remainder of division by zero");
			return FALSE;
		}
		result = vleft % vright;
		break;
	case TECO_OP_ADD:
		result = vleft + vright;
		break;
	case TECO_OP_SUB:
		result = vleft - vright;
		break;
	case TECO_OP_AND:
		result = vleft & vright;
		break;
	case TECO_OP_XOR:
		result = vleft ^ vright;
		break;
	case TECO_OP_OR:
		result = vleft | vright;
		break;
	default:
		/* shouldn't happen */
		g_assert_not_reached();
	}

	teco_expressions_push(result);
	return TRUE;
}

/**
 * Resolve all operations on the top of the stack.
 *
 * @param pop_brace If TRUE this also pops the "brace" operator.
 * @param error A GError
 * @return FALSE if an error occurred
 */
gboolean
teco_expressions_eval(gboolean pop_brace, GError **error)
{
	for (;;) {
		gint n = teco_expressions_first_op();
		if (n < 0)
			break;

		teco_operator_t op = teco_expressions_peek_op(n);
		if (op == TECO_OP_BRACE) {
			if (pop_brace)
				teco_expressions_pop_op(n);
			break;
		}
		if (n < 1)
			break;

		if (!teco_expressions_calc(error))
			return FALSE;
	}

	return TRUE;
}

/**
 * Get number of numeric arguments on the top of the stack.
 *
 * @fixme You must call teco_expressions_eval() to resolve operations
 * before this gives sensitive results.
 * Overall it might be better to automatically call teco_expressions_eval()
 * here or introduce a separate teco_expressions_args_calc().
 */
guint
teco_expressions_args(void)
{
	guint n = 0;

	while (n < teco_operators->len && teco_expressions_peek_op(n) == TECO_OP_NUMBER)
		n++;

	return n;
}

gint
teco_expressions_first_op(void)
{
	for (guint i = 0; i < teco_operators->len; i++) {
		switch (teco_expressions_peek_op(i)) {
		case TECO_OP_NUMBER:
		case TECO_OP_NEW:
			break;
		default:
			return i;
		}
	}

	return -1; /* no operator */
}

gboolean
teco_expressions_discard_args(GError **error)
{
	if (!teco_expressions_eval(FALSE, error))
		return FALSE;
	for (guint i = teco_expressions_args(); i; i--)
		if (!teco_expressions_pop_num_calc(NULL, 0, error))
			return FALSE;
	return TRUE;
}

/** The nesting level of braces */
guint teco_brace_level = 0;

void
teco_expressions_brace_open(void)
{
	while (teco_operators->len > 0 && teco_expressions_peek_op(0) == TECO_OP_NEW)
		teco_expressions_pop_op(0);

	teco_expressions_push_op(TECO_OP_BRACE);
	teco_undo_guint(teco_brace_level)++;
}

gboolean
teco_expressions_brace_return(guint keep_braces, guint args, GError **error)
{
	g_autofree teco_int_t *return_numbers = g_new(teco_int_t, args);

	for (guint i = args; i; i--)
		return_numbers[i-1] = teco_expressions_pop_num(0);

	teco_undo_guint(teco_brace_level);

	while (teco_brace_level > keep_braces) {
		if (!teco_expressions_discard_args(error) ||
		    !teco_expressions_eval(TRUE, error))
			return FALSE;
		teco_brace_level--;
	}

	for (guint i = 0; i < args; i++)
		teco_expressions_push(return_numbers[i]);

	return TRUE;
}

gboolean
teco_expressions_brace_close(GError **error)
{
	if (!teco_brace_level) {
		g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
		                    "Missing opening brace");
		return FALSE;
	}
	teco_undo_guint(teco_brace_level)--;
	return teco_expressions_eval(TRUE, error);
}

void
teco_expressions_clear(void)
{
	g_array_set_size(teco_numbers, 0);
	g_array_set_size(teco_operators, 0);
	teco_brace_level = 0;
}

/**
 * Format a TECO integer as the `\` command would.
 *
 * @param buffer The output buffer of at least TECO_EXPRESSIONS_FORMAT_LEN characters.
 *               The output string will be null-terminated.
 * @param number The number to format.
 * @param qreg The radix register (^R).
 * @return A pointer into buffer to the beginning of the formatted number.
 */
gchar *
teco_expressions_format(gchar *buffer, teco_int_t number, teco_qreg_t *qreg)
{
	assert(qreg != NULL);
	teco_int_t radix = 10;
	qreg->vtable->get_integer(qreg, &radix, NULL);

	gchar *p = buffer + TECO_EXPRESSIONS_FORMAT_LEN;

	teco_int_t v = number;

	*--p = '\0';
	do {
		*--p = '0' + ABS(v % radix);
		if (*p > '9')
			*p += 'A' - '9' - 1;
	} while ((v /= radix));
	if (number < 0)
		*--p = '-';

	return p;
}

static void TECO_DEBUG_CLEANUP
teco_expressions_cleanup(void)
{
	g_array_free(teco_numbers, TRUE);
	g_array_free(teco_operators, TRUE);
}
