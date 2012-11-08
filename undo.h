#ifndef __UNDO_H
#define __UNDO_H

#include <bsd/sys/queue.h>

#include <glib.h>
#include <glib/gprintf.h>

#include <Scintilla.h>

class UndoToken {
public:
	SLIST_ENTRY(UndoToken) tokens;

	gint pos;

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

	void push_msg(unsigned int iMessage,
		      uptr_t wParam = 0, sptr_t lParam = 0);

	template <typename Type>
	inline void
	push_var(Type &variable, Type value)
	{
		push(new UndoTokenVariable<Type>(variable, value));
	}

	template <typename Type>
	inline void
	push_var(Type &variable)
	{
		push_var<Type>(variable, variable);
	}

	inline void
	push_str(gchar *&variable, gchar *str)
	{
		push(new UndoTokenString(variable, str));
	}
	inline void
	push_str(gchar *&variable)
	{
		push_str(variable, variable);
	}

	void pop(gint pos);

	void clear(void);
} undo;

#endif