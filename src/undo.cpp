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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <bsd/sys/queue.h>

#include <glib.h>
#include <glib/gstdio.h>

#include <Scintilla.h>

#include "sciteco.h"
#include "cmdline.h"
#include "interface.h"
#include "undo.h"

namespace SciTECO {

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
		token->pos = cmdline_pos;
		SLIST_INSERT_HEAD(&head, token, tokens);
	} else {
		delete token;
	}
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

} /* namespace SciTECO */
