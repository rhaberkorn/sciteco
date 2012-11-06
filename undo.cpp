#include <string.h>
#include <bsd/sys/queue.h>

#include <glib.h>

#include <Scintilla.h>

#include "sciteco.h"
#include "undo.h"

UndoStack undo;

UndoToken::UndoToken() : pos(strlen(cmdline)) {}

void
UndoTokenMessage::run(void)
{
	editor_msg(iMessage, wParam, lParam);
}

void
UndoStack::push_msg(unsigned int iMessage, uptr_t wParam, sptr_t lParam)
{
	UndoToken *token = new UndoTokenMessage(iMessage, wParam, lParam);
	SLIST_INSERT_HEAD(&head, token, tokens);
}

#if 0
template <typename Type>
void
UndoStack::push_var(Type &variable, Type value)
{
	UndoToken *token = new UndoTokenVariable<Type>(variable, value);
	SLIST_INSERT_HEAD(&head, token, tokens);
}
#endif

void
UndoStack::pop(gint pos)
{
	while (!SLIST_EMPTY(&head) && SLIST_FIRST(&head)->pos >= pos) {
		UndoToken *top = SLIST_FIRST(&head);

		top->run();

		SLIST_REMOVE_HEAD(&head, tokens);
		delete top;
	}
}

UndoStack::~UndoStack()
{
	UndoToken *token, *next;

	SLIST_FOREACH_SAFE(token, &head, tokens, next)
		delete token;
}
