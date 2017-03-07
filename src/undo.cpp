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
#include "undo.h"

namespace SciTECO {

//#define DEBUG

UndoStack undo;

void
UndoStack::push(UndoToken *token)
{
	/*
	 * All undo token allocations should take place using the
	 * variadic template version of UndoStack::push(), so we
	 * don't have to check `enabled` here.
	 */
	g_assert(enabled == true);

#ifdef DEBUG
	g_printf("UNDO PUSH %p\n", token);
#endif

	/*
	 * There can very well be 0 undo tokens
	 * per input character (e.g. NOPs like space).
	 */
	while (heads->len <= cmdline.pc)
		g_ptr_array_add(heads, NULL);
	g_assert(heads->len == cmdline.pc+1);

	SLIST_NEXT(token, tokens) =
		(UndoToken *)g_ptr_array_index(heads, heads->len-1);
	g_ptr_array_index(heads, heads->len-1) = token;
}

void
UndoStack::pop(gint pc)
{
	while ((gint)heads->len > pc) {
		UndoToken *top =
			(UndoToken *)g_ptr_array_remove_index(heads, heads->len-1);

		while (top) {
			UndoToken *next = SLIST_NEXT(top, tokens);

#ifdef DEBUG
			g_printf("UNDO POP %p\n", top);
			fflush(stdout);
#endif
			top->run();

			delete top;
			top = next;
		}
	}
}

void
UndoStack::clear(void)
{
	while (heads->len) {
		UndoToken *top =
			(UndoToken *)g_ptr_array_remove_index(heads, heads->len-1);

		while (top) {
			UndoToken *next = SLIST_NEXT(top, tokens);
			delete top;
			top = next;
		}
	}
}

} /* namespace SciTECO */
