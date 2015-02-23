/*
 * Copyright (C) 2012-2015 Robin Haberkorn
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
#include "error.h"
#include "undo.h"

namespace SciTECO {

//#define DEBUG

UndoStack undo;

static inline gdouble
size_to_mb(gsize s)
{
	return ((gdouble)s)/(1024*1024);
}

void
UndoStack::set_memory_limit(gsize new_limit)
{
	if (new_limit) {
		if (!memory_limit) {
			/* memory_usage outdated - recalculate */
			UndoToken *token;

			memory_usage = 0;
			SLIST_FOREACH(token, &head, tokens)
				memory_usage += token->get_size();
		}

		if (memory_usage > new_limit)
			throw Error("Cannot set undo memory limit (%gmb): "
			            "Current stack too large (%gmb).",
			            size_to_mb(new_limit),
			            size_to_mb(memory_usage));
	}

	push_var(memory_limit) = new_limit;
}

void
UndoStack::push(UndoToken *token)
{
	if (!enabled) {
		delete token;
		return;
	}

	if (memory_limit) {
		gsize token_size = token->get_size();

		if (memory_usage + token_size > memory_limit) {
			delete token;
			throw Error("Undo stack memory limit (%gmb) exceeded. "
			            "See <EJ> command.",
			            size_to_mb(memory_limit));
		}

		memory_usage += token_size;
	}

#ifdef DEBUG
	g_printf("UNDO PUSH %p\n", token);
#endif
	token->pos = cmdline_pos;
	SLIST_INSERT_HEAD(&head, token, tokens);
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

		if (memory_limit)
			memory_usage -= top->get_size();
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

	memory_usage = 0;
}

UndoStack::~UndoStack()
{
	UndoToken *token, *next;

	SLIST_FOREACH_SAFE(token, &head, tokens, next)
		delete token;
}

} /* namespace SciTECO */
