#ifndef __UNDO_H
#define __UNDO_H

#include <bsd/sys/queue.h>

#include <glib.h>

#include <Scintilla.h>

class UndoToken {
public:
	SLIST_ENTRY(UndoToken) tokens;

	gint pos;

	UndoToken();

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

extern class UndoStack {
	SLIST_HEAD(undo_head, UndoToken) head;

public:
	UndoStack()
	{
		SLIST_INIT(&head);
	}
	~UndoStack();

	void push_msg(unsigned int iMessage,
		      uptr_t wParam = 0, sptr_t lParam = 0);

	template <typename Type>
	void
	push_var(Type &variable, Type value)
	{
		UndoToken *token = new UndoTokenVariable<Type>(variable, value);
		SLIST_INSERT_HEAD(&head, token, tokens);
	}

	template <typename Type>
	inline void
	push_var(Type &variable)
	{
		push_var<Type>(variable, variable);
	}

	void pop(gint pos);
} undo;

#endif