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

#ifndef __UNDO_H
#define __UNDO_H

#include <bsd/sys/queue.h>

#include <glib.h>
#include <glib/gprintf.h>

#include <Scintilla.h>

#ifdef DEBUG
#include "parser.h"
#endif

class UndoToken {
public:
	SLIST_ENTRY(UndoToken) tokens;

	gint pos;

	virtual ~UndoToken() {}

	virtual void run() = 0;
};

class UndoTokenMessage : public UndoToken {
	unsigned int iMessage;
	uptr_t wParam;
	sptr_t lParam;

public:
	UndoTokenMessage(unsigned int _iMessage,
			 uptr_t _wParam = 0, sptr_t _lParam = 0)
			: UndoToken(), iMessage(_iMessage),
			  wParam(_wParam), lParam(_lParam) {}

	void run(void);
};

template <typename Type>
class UndoTokenVariable : public UndoToken {
	Type *ptr;
	Type value;

public:
	UndoTokenVariable(Type &variable, Type _value)
			 : UndoToken(), ptr(&variable), value(_value) {}

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
		       : UndoToken(), ptr(&variable)
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
		       : UndoToken(), ptr(&variable), obj(_obj) {}

	~UndoTokenObject()
	{
		if (obj)
			delete obj;
	}

	void
	run(void)
	{
		if (*ptr)
			delete *ptr;
		*ptr = obj;
		obj = NULL;
	}
};

extern class UndoStack {
	SLIST_HEAD(Head, UndoToken) head;

public:
	bool enabled;

	UndoStack(bool _enabled = false) : enabled(_enabled)
	{
		SLIST_INIT(&head);
	}
	~UndoStack();

	void push(UndoToken *token);

	inline void
	push_msg(unsigned int iMessage,
		 uptr_t wParam = 0, sptr_t lParam = 0)
	{
		push(new UndoTokenMessage(iMessage, wParam, lParam));
	}

	template <typename Type>
	inline Type &
	push_var(Type &variable, Type value)
	{
		push(new UndoTokenVariable<Type>(variable, value));
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
		push(new UndoTokenString(variable, str));
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
		push(new UndoTokenObject<Type>(variable, obj));
		return variable;
	}

	template <class Type>
	inline Type *&
	push_obj(Type *&variable)
	{
		return push_obj<Type>(variable, variable);
	}

	void pop(gint pos);

	void clear(void);
} undo;

#endif