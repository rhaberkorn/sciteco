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

#ifndef __UNDO_H
#define __UNDO_H

#include <string.h>

#include <bsd/sys/queue.h>

#include <glib.h>
#include <glib/gprintf.h>

#include "memory.h"

#ifdef DEBUG
#include "parser.h"
#endif

namespace SciTECO {

class UndoToken : public Object {
public:
	SLIST_ENTRY(UndoToken) tokens;

	/**
	 * Command-line character position (program counter)
	 * corresponding to this token.
	 *
	 * @todo This wastes memory in macro calls and loops
	 *       because all undo tokens will have the same
	 *       value. It may be better to redesign the undo
	 *       stack data structure - as a list/array pointing
	 *       to undo stacks per character.
	 */
	gint pc;

	virtual ~UndoToken() {}

	virtual void run(void) = 0;
};

template <typename Type>
class UndoTokenVariable : public UndoToken {
	Type *ptr;
	Type value;

public:
	UndoTokenVariable(Type &variable, Type _value)
			 : ptr(&variable), value(_value) {}

	void
	run(void)
	{
#ifdef DEBUG
		if ((State **)ptr == &States::current)
			g_printf("undo state -> %p\n", (void *)value);
#endif
		*ptr = value;
	}
};

class UndoTokenString : public UndoToken {
	gchar **ptr;
	gchar *str;

public:
	UndoTokenString(gchar *&variable, gchar *_str)
		       : ptr(&variable)
	{
		str = _str ? g_strdup(_str) : NULL;
	}

	~UndoTokenString()
	{
		g_free(str);
	}

	void
	run(void)
	{
		g_free(*ptr);
		*ptr = str;
		str = NULL;
	}
};

template <class Type>
class UndoTokenObject : public UndoToken {
	Type **ptr;
	Type *obj;

public:
	UndoTokenObject(Type *&variable, Type *_obj)
		       : ptr(&variable), obj(_obj) {}

	~UndoTokenObject()
	{
		delete obj;
	}

	void
	run(void)
	{
		delete *ptr;
		*ptr = obj;
		obj = NULL;
	}
};

extern class UndoStack : public Object {
	SLIST_HEAD(Head, UndoToken) head;

	void push(UndoToken *token);

public:
	bool enabled;

	UndoStack(bool _enabled = false) : enabled(_enabled)
	{
		SLIST_INIT(&head);
	}
	~UndoStack();

	/**
	 * Allocate and push undo token.
	 *
	 * This does nothing if undo is disabled and should
	 * not be used when ownership of some data is to be
	 * passed to the undo token.
	 */
	template <class TokenType, typename... Params>
	inline void
	push(Params && ... params)
	{
		if (enabled)
			push(new TokenType(params...));
	}

	/**
	 * Allocate and push undo token, passing ownership.
	 *
	 * This creates and deletes the undo token cheaply
	 * if undo is disabled, so that data whose ownership
	 * is passed to the undo token is correctly reclaimed.
	 *
	 * @bug We must know which version of push to call
	 * depending on the token type. This could be hidden
	 * if UndoTokens had static push methods that take care
	 * of reclaiming memory.
	 */
	template <class TokenType, typename... Params>
	inline void
	push_own(Params && ... params)
	{
		if (enabled) {
			push(new TokenType(params...));
		} else {
			/* ensures that all memory is reclaimed */
			TokenType dummy(params...);
		}
	}

	template <typename Type>
	inline Type &
	push_var(Type &variable, Type value)
	{
		push<UndoTokenVariable<Type>>(variable, value);
		return variable;
	}

	template <typename Type>
	inline Type &
	push_var(Type &variable)
	{
		return push_var<Type>(variable, variable);
	}

	inline gchar *&
	push_str(gchar *&variable, gchar *str)
	{
		push<UndoTokenString>(variable, str);
		return variable;
	}
	inline gchar *&
	push_str(gchar *&variable)
	{
		return push_str(variable, variable);
	}

	template <class Type>
	inline Type *&
	push_obj(Type *&variable, Type *obj)
	{
		/* pass ownership of original object */
		push_own<UndoTokenObject<Type>>(variable, obj);
		return variable;
	}

	template <class Type>
	inline Type *&
	push_obj(Type *&variable)
	{
		return push_obj<Type>(variable, variable);
	}

	void pop(gint pc);

	void clear(void);
} undo;

} /* namespace SciTECO */

#endif
