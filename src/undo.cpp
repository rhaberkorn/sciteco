#include <stdio.h>
#include <string.h>
#include <bsd/sys/queue.h>

#include <glib.h>
#include <glib/gstdio.h>

#include <Scintilla.h>

#include "sciteco.h"
#include "interface.h"
#include "undo.h"

//#define DEBUG

UndoStack undo;

void
UndoTokenMessage::run(void)
{
	interface.ssm(iMessage, wParam, lParam);
}

void
UndoStack::push(UndoToken *token)
{
	if (enabled) {
#ifdef DEBUG
		g_printf("UNDO PUSH %p\n", token);
#endif
		token->pos = strlen(cmdline);
		SLIST_INSERT_HEAD(&head, token, tokens);
	} else {
		delete token;
	}
}

void
UndoStack::push_msg(unsigned int iMessage, uptr_t wParam, sptr_t lParam)
{
	push(new UndoTokenMessage(iMessage, wParam, lParam));
}

void
UndoStack::pop(gint pos)
{
	while (!SLIST_EMPTY(&head) && SLIST_FIRST(&head)->pos >= pos) {
		UndoToken *top = SLIST_FIRST(&head);
#ifdef DEBUG
		g_printf("UNDO POP %p\n", top);
		fflush(stdout);
#endif

		top->run();

		SLIST_REMOVE_HEAD(&head, tokens);
		delete top;
	}
}

void
UndoStack::clear(void)
{
	UndoToken *cur;

	while ((cur = SLIST_FIRST(&head))) {
		SLIST_REMOVE_HEAD(&head, tokens);
		delete cur;
	}
}

UndoStack::~UndoStack()
{
	UndoToken *token, *next;

	SLIST_FOREACH_SAFE(token, &head, tokens, next)
		delete token;
}
