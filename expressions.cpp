#include <glib.h>

#include "sciteco.h"
#include "undo.h"
#include "expressions.h"

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

gint64
Expressions::push(gint64 number)
{
	while (numbers.items() > 0 && numbers.peek() == G_MAXINT64)
		pop_num();

	push(OP_NUMBER);

	undo.push(new UndoTokenPop<gint64>(numbers));
	return numbers.push(number);
}

gint64
Expressions::pop_num(int index)
{
	gint64 n = G_MAXINT64;

	pop_op();
	if (numbers.items() > 0) {
		n = numbers.pop(index);
		undo.push(new UndoTokenPush<gint64>(numbers, n, index));
	}

	return n;
}

gint64
Expressions::pop_num_calc(int index, gint64 imply)
{
	gint64 n = G_MAXINT64;

	eval();
	if (args() > 0)
		n = pop_num(index);
	if (n == G_MAXINT64)
		n = imply;

	if (num_sign < 0)
		set_num_sign(1);

	return n;
}

gint64
Expressions::add_digit(gchar digit)
{
	gint64 n = 0;

	if (args() > 0) {
		n = pop_num();
		if (n == G_MAXINT64)
			n = 0;
	}

	return push(n*radix + num_sign*(digit - '0'));
}

Expressions::Operator
Expressions::push(Expressions::Operator op)
{
	undo.push(new UndoTokenPop<Operator>(operators));
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
		undo.push(new UndoTokenPush<Operator>(operators, op, index));
	}

	return op;
}

void
Expressions::calc(void)
{
	gint64 result;

	gint64 vright = pop_num();
	Operator op = pop_op();
	gint64 vleft = pop_num();

	switch (op) {
	case OP_POW:
		result = 1;
		while (vright--)
			result *= vleft;
		break;
	case OP_MUL: result = vleft * vright; break;
	case OP_DIV: result = vleft / vright; break;
	case OP_MOD: result = vleft % vright; break;
	case OP_ADD: result = vleft + vright; break;
	case OP_SUB: result = vleft - vright; break;
	case OP_AND: result = vleft & vright; break;
	case OP_OR:  result = vleft | vright; break;
	default:
		/* shouldn't happen */
		g_assert(false);
	}

	push(result);
}

void
Expressions::eval(bool pop_brace)
{
	if (numbers.items() < 2)
		return;

	for (;;) {
		gint n = first_op();
		Operator op;

		if (!n)
			break;

		op = operators.peek(n);
		if (op == OP_LOOP)
			break;
		if (op == OP_BRACE) {
			if (pop_brace)
				pop_op(n);
			break;
		}

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

void
Expressions::discard_args(void)
{
	eval();
	for (int i = args(); i; i--)
		pop_num_calc();
}
