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

#ifndef __UNDO_H
#define __UNDO_H

#include <string.h>

#include <bsd/sys/queue.h>

#include <glib.h>
#include <glib/gprintf.h>

#ifdef DEBUG
#include "parser.h"
#endif

namespace SciTECO {

/**
 * Default undo stack memory limit (500mb).
 */
#define UNDO_MEMORY_LIMIT_DEFAULT (500*1024*1024)

class UndoToken {
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

	/**
	 * Return approximated size of this object.
	 * If possible it should take all heap objects
	 * into account that are memory-managed by the undo
	 * token.
	 * For a simple implementation, you may derive
	 * from UndoTokenWithSize.
	 */
	virtual gsize get_size(void) const = 0;
};

/**
 * UndoToken base class employing the CRTP idiom.
 * Deriving this class adds a size approximation based
 * on the shallow size of the template parameter.
 */
template <class UndoTokenImpl>
class UndoTokenWithSize : public UndoToken {
public:
	gsize
	get_size(void) const
	{
		return sizeof(UndoTokenImpl);
	}
};

template <typename Type>
class UndoTokenVariable : public UndoTokenWithSize< UndoTokenVariable<Type> > {
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

	gsize
	get_size(void) const
	{
		return str ? sizeof(*this) + strlen(str) + 1
		           : sizeof(*this);
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

	gsize
	get_size(void) const
	{
		return obj ? sizeof(*this) + sizeof(*obj)
		           : sizeof(*this);
	}
};

extern class UndoStack {
	SLIST_HEAD(Head, UndoToken) head;

	/**
	 * Current approx. memory usage of all
	 * undo tokens in the stack.
	 * It is only up to date if memory limiting
	 * is enabled.
	 */
	gsize memory_usage;

public:
	bool enabled;

	/**
	 * Undo stack memory limit in bytes.
	 * 0 means no limiting.
	 */
	gsize memory_limit;

	UndoStack(bool _enabled = false)
	         : memory_usage(0), enabled(_enabled),
	           memory_limit(UNDO_MEMORY_LIMIT_DEFAULT)
	{
		SLIST_INIT(&head);
	}
	~UndoStack();

	void set_memory_limit(gsize new_limit = 0);

	void push(UndoToken *token);

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

	void pop(gint pc);

	void clear(void);
} undo;

} /* namespace SciTECO */

#endif
