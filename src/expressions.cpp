/*
 * Copyright (C) 2012-2014 Robin Haberkorn
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
#include "undo.h"
#include "parser.h" // State::Error
#include "expressions.h"

namespace SciTECO {

Expressions expressions;

void
Expressions::set_num_sign(gint sign)
{
	undo.push_var<gint>(num_sign);
	num_sign = sign;
}

void
Expressions::set_radix(gint r)
{
	undo.push_var<gint>(radix);
	radix = r;
}

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

	numbers.undo_pop();
	return numbers.push(number);
}

tecoInt
Expressions::pop_num(int index)
{
	tecoInt n = 0;
	Operator op = pop_op();

	g_assert(op == OP_NUMBER);

	if (numbers.items() > 0) {
		n = numbers.pop(index);
		numbers.undo_push(n, index);
	}

	return n;
}

tecoInt
Expressions::pop_num_calc(int index, tecoInt imply)
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
	operators.undo_pop();
	return operators.push(op);
}

Expressions::Operator
Expressions::push_calc(Expressions::Operator op)
{
	int first = first_op();

	/* calculate if op has lower precedence than op on stack */
	if (first && operators.peek(first) <= op)
		calc();

	return push(op);
}

Expressions::Operator
Expressions::pop_op(int index)
{
	Operator op = OP_NIL;

	if (operators.items() > 0) {
		op = operators.pop(index);
		operators.undo_push(op, index);
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
		throw State::Error("Missing right operand");
	vright = pop_num();
	op = pop_op();
	if (operators.peek() != OP_NUMBER)
		throw State::Error("Missing left operand");
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
			throw State::Error("Division by zero");
		result = vleft / vright;
		break;
	case OP_MOD:
		if (!vright)
			throw State::Error("Remainder of division by zero");
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
	case OP_OR:
		result = vleft | vright;
		break;
	default:
		/* shouldn't happen */
		g_assert(false);
	}

	push(result);
}

void
Expressions::eval(bool pop_brace)
{
	for (;;) {
		gint n = first_op();
		Operator op;

		op = operators.peek(n);
		if (op == OP_LOOP)
			break;
		if (op == OP_BRACE) {
			if (pop_brace)
				pop_op(n);
			break;
		}
		if (n < 2)
			break;

		calc();
	}
}

int
Expressions::args(void)
{
	int n = 0;
	int items = operators.items();

	while (n < items && operators.peek(n+1) == OP_NUMBER)
		n++;

	return n;
}

int
Expressions::find_op(Operator op)
{
	int items = operators.items();

	for (int i = 1; i <= items; i++)
		if (operators.peek(i) == op)
			return i;

	return 0;
}

int
Expressions::first_op(void)
{
	int items = operators.items();

	for (int i = 1; i <= items; i++) {
		switch (operators.peek(i)) {
		case OP_NUMBER:
		case OP_NEW:
			break;
		default:
			return i;
		}
	}

	return 0;
}

void
Expressions::discard_args(void)
{
	eval();
	for (int i = args(); i; i--)
		pop_num_calc();
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
			*p += 'A' - '9';
	} while ((v /= radix));
	if (number < 0)
		*--p = '-';

	return p;
}

} /* namespace SciTECO */
