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

#include "memory.h"
#include "undo.h"
#include "error.h"

namespace SciTECO {

template <typename Type>
class ValueStack : public Object {
	/*
	 * NOTE: Since value stacks are usually singleton,
	 * we pass them as a template parameter, saving space
	 * in the undo token.
	 */
	template <ValueStack<Type> &stack>
	class UndoTokenPush : public UndoToken {
		Type	value;
		guint	index;

	public:
		UndoTokenPush(Type _value, guint _index = 0)
			     : value(_value), index(_index) {}

		void
		run(void)
		{
			stack.push(value, index);
		}
	};

	template <ValueStack<Type> &stack>
	class UndoTokenPop : public UndoToken {
		guint index;

	public:
		UndoTokenPop(guint _index = 0)
			    : index(_index) {}

		void
		run(void)
		{
			stack.pop(index);
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

	template <ValueStack<Type> &stack>
	static inline void
	undo_push(Type value, guint index = 0)
	{
		undo.push<UndoTokenPush<stack>>(value, index);
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

	template <ValueStack<Type> &stack>
	static inline void
	undo_pop(guint index = 0)
	{
		undo.push<UndoTokenPop<stack>>(index);
	}

	inline Type &
	peek(guint index = 0)
	{
		g_assert(items() > index);

		return sp[index];
	}

	/** Clear all but `keep_items` items. */
	inline void
	clear(guint keep_items = 0)
	{
		g_assert(keep_items <= items());

		sp = stack_top - keep_items;
	}
};

/**
 * Arithmetic expression stacks
 */
extern class Expressions : public Object {
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

	/*
	 * Number and operator stacks are static, so
	 * they can be passed to the undo token constructors.
	 * This is OK since Expression is singleton.
	 */
	typedef ValueStack<tecoInt> NumberStack;
	static NumberStack numbers;

	typedef ValueStack<Operator> OperatorStack;
	static OperatorStack operators;

public:
	Expressions() : num_sign(1), radix(10), brace_level(0) {}

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

	/** The nesting level of braces */
	guint brace_level;

	inline void
	brace_open(void)
	{
		push(OP_BRACE);
		undo.push_var(brace_level)++;
	}

	void brace_return(guint keep_braces, guint args = 0);

	inline void
	brace_close(void)
	{
		if (!brace_level)
			throw Error("Missing opening brace");
		undo.push_var(brace_level)--;
		eval(true);
	}

	inline void
	clear(void)
	{
		numbers.clear();
		operators.clear();
		brace_level = 0;
	}

	const gchar *format(tecoInt number);

private:
	void calc(void);

	gint first_op(void);
} expressions;

} /* namespace SciTECO */

#endif
