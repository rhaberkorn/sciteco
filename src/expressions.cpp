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

#include <glib.h>

#include "sciteco.h"
#include "error.h"
#include "expressions.h"

namespace SciTECO {

Expressions			expressions;
Expressions::NumberStack	Expressions::numbers;
Expressions::OperatorStack	Expressions::operators;

tecoInt
Expressions::push(tecoInt number)
{
	while (operators.items() && operators.peek() == OP_NEW)
		pop_op();

	push(OP_NUMBER);

	if (num_sign < 0) {
		set_num_sign(1);
		number *= -1;
	}

	NumberStack::undo_pop<numbers>();
	return numbers.push(number);
}

tecoInt
Expressions::pop_num(guint index)
{
	tecoInt n = 0;
	Operator op = pop_op();

	g_assert(op == OP_NUMBER);

	if (numbers.items()) {
		n = numbers.pop(index);
		NumberStack::undo_push<numbers>(n, index);
	}

	return n;
}

tecoInt
Expressions::pop_num_calc(guint index, tecoInt imply)
{
	eval();
	if (num_sign < 0)
		set_num_sign(1);

	return args() > 0 ? pop_num(index) : imply;
}

tecoInt
Expressions::add_digit(gchar digit)
{
	tecoInt n = args() > 0 ? pop_num() : 0;

	return push(n*radix + (n < 0 ? -1 : 1)*(digit - '0'));
}

Expressions::Operator
Expressions::push(Expressions::Operator op)
{
	OperatorStack::undo_pop<operators>();
	return operators.push(op);
}

Expressions::Operator
Expressions::push_calc(Expressions::Operator op)
{
	gint first = first_op();

	/* calculate if op has lower precedence than op on stack */
	if (first >= 0 &&
	    precedence(op) <= precedence(operators.peek(first)))
		calc();

	return push(op);
}

Expressions::Operator
Expressions::pop_op(guint index)
{
	Operator op = OP_NIL;

	if (operators.items()) {
		op = operators.pop(index);
		OperatorStack::undo_push<operators>(op, index);
	}

	return op;
}

void
Expressions::calc(void)
{
	tecoInt result;

	tecoInt vright;
	Operator op;
	tecoInt vleft;

	if (operators.peek() != OP_NUMBER)
		throw Error("Missing right operand");
	vright = pop_num();
	op = pop_op();
	if (operators.peek() != OP_NUMBER)
		throw Error("Missing left operand");
	vleft = pop_num();

	switch (op) {
	case OP_POW:
		for (result = 1; vright--; result *= vleft);
		break;
	case OP_MUL:
		result = vleft * vright;
		break;
	case OP_DIV:
		if (!vright)
			throw Error("Division by zero");
		result = vleft / vright;
		break;
	case OP_MOD:
		if (!vright)
			throw Error("Remainder of division by zero");
		result = vleft % vright;
		break;
	case OP_ADD:
		result = vleft + vright;
		break;
	case OP_SUB:
		result = vleft - vright;
		break;
	case OP_AND:
		result = vleft & vright;
		break;
	case OP_XOR:
		result = vleft ^ vright;
		break;
	case OP_OR:
		result = vleft | vright;
		break;
	default:
		/* shouldn't happen */
		g_assert_not_reached();
	}

	push(result);
}

void
Expressions::eval(bool pop_brace)
{
	for (;;) {
		gint n = first_op();
		Operator op;

		if (n < 0)
			break;

		op = operators.peek(n);
		if (op == OP_BRACE) {
			if (pop_brace)
				pop_op(n);
			break;
		}
		if (n < 1)
			break;

		calc();
	}
}

guint
Expressions::args(void)
{
	guint n = 0;
	guint items = operators.items();

	while (n < items && operators.peek(n) == OP_NUMBER)
		n++;

	return n;
}

gint
Expressions::first_op(void)
{
	guint items = operators.items();

	for (guint i = 0; i < items; i++) {
		switch (operators.peek(i)) {
		case OP_NUMBER:
		case OP_NEW:
			break;
		default:
			return i;
		}
	}

	return -1; /* no operator */
}

void
Expressions::discard_args(void)
{
	eval();
	for (guint i = args(); i; i--)
		pop_num_calc();
}

void
Expressions::brace_return(guint keep_braces, guint args)
{
	tecoInt return_numbers[args];

	for (guint i = args; i; i--)
		return_numbers[i-1] = pop_num();

	undo.push_var(brace_level);

	while (brace_level > keep_braces) {
		discard_args();
		eval(true);
		brace_level--;
	}

	for (guint i = 0; i < args; i++)
		push(return_numbers[i]);
}

const gchar *
Expressions::format(tecoInt number)
{
	/* maximum length if radix = 2 */
	static gchar buf[1+sizeof(number)*8+1];
	gchar *p = buf + sizeof(buf);

	tecoInt v = ABS(number);

	*--p = '\0';
	do {
		*--p = '0' + (v % radix);
		if (*p > '9')
			*p += 'A' - '9' - 1;
	} while ((v /= radix));
	if (number < 0)
		*--p = '-';

	return p;
}

} /* namespace SciTECO */
