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

#ifndef __EXPRESSIONS_H
#define __EXPRESSIONS_H

#include <glib.h>

#include "undo.h"
#include "error.h"

namespace SciTECO {

template <typename Type>
class ValueStack {
	class UndoTokenPush : public UndoToken {
		ValueStack<Type> *stack;

		Type	value;
		int	index;

	public:
		UndoTokenPush(ValueStack<Type> *_stack,
			      Type _value, int _index = 1)
			     : stack(_stack), value(_value), index(_index) {}

		void
		run(void)
		{
			stack->push(value, index);
		}
	};

	class UndoTokenPop : public UndoToken {
		ValueStack<Type> *stack;

		int index;

	public:
		UndoTokenPop(ValueStack<Type> *_stack, int _index = 1)
			    : stack(_stack), index(_index) {}

		void
		run(void)
		{
			stack->pop(index);
		}
	};

	int size;

	Type *stack;
	Type *top;

public:
	ValueStack(int _size = 1024) : size(_size)
	{
		top = stack = new Type[size];
	}

	~ValueStack()
	{
		delete[] stack;
	}

	inline int
	items(void)
	{
		return top - stack;
	}

	inline Type &
	push(Type value, int index = 1)
	{
		if (items() == size)
			throw Error("Stack overflow");

		for (int i = -index + 1; i; i++)
			top[i+1] = top[i];

		top++;
		return peek(index) = value;
	}
	inline void
	undo_push(Type value, int index = 1)
	{
		undo.push(new UndoTokenPush(this, value, index));
	}

	inline Type
	pop(int index = 1)
	{
		Type v = peek(index);

		top--;
		while (--index)
			top[-index] = top[-index + 1];

		return v;
	}
	inline void
	undo_pop(int index = 1)
	{
		undo.push(new UndoTokenPop(this, index));
	}

	inline Type &
	peek(int index = 1)
	{
		return top[-index];
	}

	inline void
	clear(void)
	{
		top = stack;
	}
};

/*
 * Arithmetic expression stacks
 */
extern class Expressions {
public:
	/* reflects also operator precedence */
	enum Operator {
		OP_NIL = 0,
		OP_POW,		// ^*
		OP_MUL,		// *
		OP_DIV,		// /
		OP_MOD,		// ^/
		OP_ADD,		// +
		OP_SUB,		// -
		OP_AND,		// &
		OP_XOR,		// ^#
		OP_OR,		// #
				// pseudo operators:
		OP_NEW,
		OP_BRACE,
		OP_LOOP,
		OP_NUMBER
	};

private:
	ValueStack<tecoInt>	numbers;
	ValueStack<Operator>	operators;

public:
	Expressions() : num_sign(1), radix(10) {}

	gint num_sign;
	void set_num_sign(gint sign);

	gint radix;
	void set_radix(gint r);

	tecoInt push(tecoInt number);

	inline tecoInt
	peek_num(int index = 1)
	{
		return numbers.peek(index);
	}
	tecoInt pop_num(int index = 1);
	tecoInt pop_num_calc(int index, tecoInt imply);
	inline tecoInt
	pop_num_calc(int index = 1)
	{
		return pop_num_calc(index, num_sign);
	}

	tecoInt add_digit(gchar digit);

	Operator push(Operator op);
	Operator push_calc(Operator op);
	inline Operator
	peek_op(int index = 1)
	{
		return operators.peek(index);
	}
	Operator pop_op(int index = 1);

	void eval(bool pop_brace = false);

	int args(void);

	void discard_args(void);

	int find_op(Operator op);

	inline void
	clear(void)
	{
		numbers.clear();
		operators.clear();
	}

	const gchar *format(tecoInt number);

private:
	void calc(void);

	int first_op(void);
} expressions;

} /* namespace SciTECO */

#endif
