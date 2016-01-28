/*
 * Copyright (C) 2012-2016 Robin Haberkorn
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
	class UndoTokenPush : public UndoTokenWithSize<UndoTokenPush> {
		/*
		 * FIXME: saving the UndoStack for each undo taken
		 * wastes a lot of memory
		 */
		ValueStack<Type> *stack;

		Type	value;
		guint	index;

	public:
		UndoTokenPush(ValueStack<Type> *_stack,
			      Type _value, guint _index = 0)
			     : stack(_stack), value(_value), index(_index) {}

		void
		run(void)
		{
			stack->push(value, index);
		}
	};

	class UndoTokenPop : public UndoTokenWithSize<UndoTokenPop> {
		ValueStack<Type> *stack;

		guint index;

	public:
		UndoTokenPop(ValueStack<Type> *_stack, guint _index = 0)
			    : stack(_stack), index(_index) {}

		void
		run(void)
		{
			stack->pop(index);
		}
	};

	/** Beginning of stack area */
	Type *stack;
	/** End of stack area */
	Type *stack_top;

	/** Pointer to top element on stack */
	Type *sp;

public:
	ValueStack(gsize size = 1024)
	{
		stack = new Type[size];
		/* stack grows to smaller addresses */
		sp = stack_top = stack+size;
	}

	~ValueStack()
	{
		delete[] stack;
	}

	inline guint
	items(void)
	{
		return stack_top - sp;
	}

	inline Type &
	push(Type value, guint index = 0)
	{
		if (G_UNLIKELY(sp == stack))
			throw Error("Stack overflow");

		/* reserve space for new element */
		sp--;
		g_assert(items() > index);

		/* move away elements after index (index > 0) */
		for (guint i = 0; i < index; i++)
			sp[i] = sp[i+1];

		return sp[index] = value;
	}
	inline void
	undo_push(Type value, guint index = 0)
	{
		undo.push(new UndoTokenPush(this, value, index));
	}

	inline Type
	pop(guint index = 0)
	{
		/* peek() already asserts */
		Type v = peek(index);

		/* elements after index are moved to index (index > 0) */
		while (index--)
			sp[index+1] = sp[index];

		/* free space of element to pop */
		sp++;

		return v;
	}
	inline void
	undo_pop(guint index = 0)
	{
		undo.push(new UndoTokenPop(this, index));
	}

	inline Type &
	peek(guint index = 0)
	{
		g_assert(items() > index);

		return sp[index];
	}

	inline void
	clear(void)
	{
		sp = stack_top;
	}
};

/**
 * Arithmetic expression stacks
 */
extern class Expressions {
public:
	/**
	 * Operator type.
	 * The enumeration value divided by 16 represents
	 * its precedence (small values mean low precedence).
	 * In other words, the value's lower nibble is
	 * reserved for enumerating operators of the
	 * same precedence.
	 */
	enum Operator {
		/*
		 * Pseudo operators
		 */
		OP_NIL	= 0x00,
		OP_NEW,
		OP_BRACE,
		OP_LOOP,
		OP_NUMBER,
		/*
		 * Real operators
		 */
		OP_POW	= 0x60,	// ^*
		OP_MOD	= 0x50,	// ^/
		OP_DIV,		// /
		OP_MUL,		// *
		OP_SUB	= 0x40,	// -
		OP_ADD,		// +
		OP_AND	= 0x30,	// &
		OP_XOR	= 0x20,	// ^#
		OP_OR	= 0x10	// #
	};

private:
	/** Get operator precedence */
	inline gint
	precedence(Operator op)
	{
		return op >> 4;
	}

	ValueStack<tecoInt>	numbers;
	ValueStack<Operator>	operators;

public:
	Expressions() : num_sign(1), radix(10) {}

	gint num_sign;
	inline void
	set_num_sign(gint sign)
	{
		undo.push_var(num_sign) = sign;
	}

	gint radix;
	inline void
	set_radix(gint r)
	{
		undo.push_var(radix) = r;
	}

	tecoInt push(tecoInt number);

	/**
	 * Push characters of a C-string.
	 * Could be overloaded on push(tecoInt)
	 * but this confuses GCC.
	 */
	inline void
	push_str(const gchar *str)
	{
		while (*str)
			push(*str++);
	}

	inline tecoInt
	peek_num(guint index = 0)
	{
		return numbers.peek(index);
	}
	tecoInt pop_num(guint index = 0);
	tecoInt pop_num_calc(guint index, tecoInt imply);
	inline tecoInt
	pop_num_calc(guint index = 0)
	{
		return pop_num_calc(index, num_sign);
	}

	tecoInt add_digit(gchar digit);

	Operator push(Operator op);
	Operator push_calc(Operator op);
	inline Operator
	peek_op(guint index = 0)
	{
		return operators.peek(index);
	}
	Operator pop_op(guint index = 0);

	void eval(bool pop_brace = false);

	guint args(void);

	void discard_args(void);

	gint find_op(Operator op);

	inline void
	clear(void)
	{
		numbers.clear();
		operators.clear();
	}

	const gchar *format(tecoInt number);

private:
	void calc(void);

	gint first_op(void);
} expressions;

} /* namespace SciTECO */

#endif
