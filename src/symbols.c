/*
 * Copyright (C) 2012-2024 Robin Haberkorn
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

#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include <Scintilla.h>
#ifdef HAVE_LEXILLA
#include <Lexilla.h>
#endif

#include "sciteco.h"
#include "string-utils.h"
#include "error.h"
#include "parser.h"
#include "core-commands.h"
#include "undo.h"
#include "expressions.h"
#include "interface.h"
#include "symbols.h"

teco_symbol_list_t teco_symbol_list_scintilla = {NULL, 0};
teco_symbol_list_t teco_symbol_list_scilexer = {NULL, 0};

/*
 * FIXME: Could be static.
 */
TECO_DEFINE_UNDO_OBJECT_OWN(scintilla_message, teco_machine_scintilla_t, /* don't delete */);

/** @memberof teco_symbol_list_t */
void
teco_symbol_list_init(teco_symbol_list_t *ctx, const teco_symbol_entry_t *entries, gint size,
                      gboolean case_sensitive)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->entries = entries;
	ctx->size = size;
	ctx->cmp_fnc = case_sensitive ? strncmp : g_ascii_strncasecmp;
}

/**
 * @note Since symbol lists are presorted constant arrays we can do a simple
 * binary search.
 * This does not use bsearch() since we'd have to prepend `prefix` in front of
 * the name.
 *
 * @memberof teco_symbol_list_t
 */
gint
teco_symbol_list_lookup(teco_symbol_list_t *ctx, const gchar *name, const gchar *prefix)
{
	gsize name_len = strlen(name);

	gsize prefix_skip = strlen(prefix);
	if (!ctx->cmp_fnc(name, prefix, prefix_skip))
		prefix_skip = 0;

	gint left = 0;
	gint right = ctx->size - 1;

	while (left <= right) {
		gint cur = left + (right-left)/2;
		gint cmp = ctx->cmp_fnc(ctx->entries[cur].name + prefix_skip,
				        name, name_len + 1);

		if (!cmp)
			return ctx->entries[cur].value;

		if (cmp > 0)
			right = cur-1;
		else /* cmp < 0 */
			left = cur+1;
	}

	return -1;
}

/**
 * Auto-complete a Scintilla symbol.
 *
 * @param ctx The symbol list.
 * @param symbol The symbol to auto-complete or NULL.
 * @param insert String to initialize with the completion.
 * @return TRUE in case of an unambiguous completion.
 *
 * @memberof teco_symbol_list_t
 */
gboolean
teco_symbol_list_auto_complete(teco_symbol_list_t *ctx, const gchar *symbol, teco_string_t *insert)
{
	memset(insert, 0, sizeof(*insert));

	if (!symbol)
		symbol = "";
	gsize symbol_len = strlen(symbol);

	if (G_UNLIKELY(!ctx->list))
		for (gint i = ctx->size; i; i--)
			ctx->list = g_list_prepend(ctx->list, (gchar *)ctx->entries[i-1].name);

	/* NOTE: element data must not be freed */
	g_autoptr(GList) glist = g_list_copy(ctx->list);
	guint glist_len = 0;

	gsize prefix_len = 0;

	for (GList *entry = g_list_first(glist), *next = g_list_next(entry);
	     entry != NULL;
	     entry = next, next = entry ? g_list_next(entry) : NULL) {
		if (g_ascii_strncasecmp(entry->data, symbol, symbol_len) != 0) {
			glist = g_list_delete_link(glist, entry);
			continue;
		}

		teco_string_t glist_str;
		glist_str.data = (gchar *)glist->data + symbol_len;
		glist_str.len = strlen(glist_str.data);

		gsize len = teco_string_casediff(&glist_str, (gchar *)entry->data + symbol_len,
		                                 strlen(entry->data) - symbol_len);
		if (!prefix_len || len < prefix_len)
			prefix_len = len;

		glist_len++;
	}

	if (prefix_len > 0) {
		teco_string_init(insert, (gchar *)glist->data + symbol_len, prefix_len);
	} else if (glist_len > 1) {
		for (GList *entry = g_list_first(glist);
		     entry != NULL;
		     entry = g_list_next(entry)) {
			teco_interface_popup_add(TECO_POPUP_PLAIN, entry->data,
			                         strlen(entry->data), FALSE);
		}

		teco_interface_popup_show();
	}

	return glist_len == 1;
}

/*
 * Command states
 */

/*
 * FIXME: This state could be static.
 */
TECO_DECLARE_STATE(teco_state_scintilla_lparam);

static gboolean
teco_scintilla_parse_symbols(teco_machine_scintilla_t *scintilla, const teco_string_t *str, GError **error)
{
	if (teco_string_contains(str, '\0')) {
		g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
		                    "Scintilla symbol names must not contain null-byte");
		return FALSE;
	}

	g_auto(GStrv) symbols = g_strsplit(str->data, ",", -1);

	if (!symbols[0])
		return TRUE;
	if (*symbols[0]) {
		gint v = teco_symbol_list_lookup(&teco_symbol_list_scintilla, symbols[0], "SCI_");
		if (v < 0) {
			g_set_error(error, TECO_ERROR, TECO_ERROR_FAILED,
			            "Unknown Scintilla message symbol \"%s\"",
				    symbols[0]);
			return FALSE;
		}
		scintilla->iMessage = v;
	}

	if (!symbols[1])
		return TRUE;
	if (*symbols[1]) {
		gint v = teco_symbol_list_lookup(&teco_symbol_list_scilexer, symbols[1], "");
		if (v < 0) {
			g_set_error(error, TECO_ERROR, TECO_ERROR_FAILED,
			            "Unknown Lexilla style symbol \"%s\"",
				    symbols[1]);
			return FALSE;
		}
		scintilla->wParam = v;
	}

	return TRUE;
}

static teco_state_t *
teco_state_scintilla_symbols_done(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	if (ctx->mode > TECO_MODE_NORMAL)
		return &teco_state_scintilla_lparam;

	/*
	 * NOTE: This is more memory efficient than pushing the individual
	 * members of teco_machine_scintilla_t and we won't need to define
	 * undo methods for the Scintilla types.
	 */
	if (ctx->parent.must_undo)
		teco_undo_object_scintilla_message_push(&ctx->scintilla);
	memset(&ctx->scintilla, 0, sizeof(ctx->scintilla));

	if ((str->len > 0 && !teco_scintilla_parse_symbols(&ctx->scintilla, str, error)) ||
	    !teco_expressions_eval(FALSE, error))
		return NULL;

	teco_int_t value;

	if (!ctx->scintilla.iMessage) {
		if (!teco_expressions_args()) {
			g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
			                    "<ES> command requires at least a message code");
			return NULL;
		}

		if (!teco_expressions_pop_num_calc(&value, 0, error))
			return NULL;
		ctx->scintilla.iMessage = value;
	}
	if (!ctx->scintilla.wParam) {
		if (!teco_expressions_pop_num_calc(&value, 0, error))
			return NULL;
		ctx->scintilla.wParam = value;
	}

	return &teco_state_scintilla_lparam;
}

/* in cmdline.c */
gboolean teco_state_scintilla_symbols_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx, gunichar key, GError **error);

/*$ ES scintilla message
 * -- Send Scintilla message
 * [lParam,][wParam,][message]ES[message][,wParam]$[lParam]$ -> result
 *
 * Send Scintilla message with code specified by
 * <message>, <wParam> and <lParam>.
 * <message> and <wParam> may be a symbolic names when specified as
 * part of the first string argument.
 * If not, they are popped from the stack.
 * <lParam> may be specified as a constant string whose
 * pointer is passed to Scintilla if specified as the second
 * string argument.
 * It is automatically null-terminated.
 * If the second string argument is empty, <lParam> is popped
 * from the stack instead.
 * Parameters popped from the stack may be omitted, in which
 * case 0 is implied.
 * The message's return value is pushed onto the stack.
 *
 * All messages defined by Scintilla (as C macros in Scintilla.h)
 * can be used by passing their name as a string to ES
 * (e.g. ESSCI_LINESONSCREEN...).
 * The \(lqSCI_\(rq prefix may be omitted and message symbols
 * are case-insensitive.
 * Only the Lexilla style names (SCE_...)
 * may be used symbolically with the ES command as <wParam>.
 * In interactive mode, symbols may be auto-completed by
 * pressing Tab.
 * String-building characters are by default interpreted
 * in the string arguments.
 *
 * As a special exception, you can and must specify a
 * Lexilla lexer name as a string argument for the \fBSCI_SETILEXER\fP
 * message, i.e. in order to load a Lexilla lexer
 * (this works similar to the old \fBSCI_SETLEXERLANGUAGE\fP message).
 *
 * .BR Warning :
 * Almost all Scintilla messages may be dispatched using
 * this command.
 * \*(ST does not keep track of the editor state changes
 * performed by these commands and cannot undo them.
 * You should never use it to change the editor state
 * (position changes, deletions, etc.) or otherwise
 * rub out will result in an inconsistent editor state.
 * There are however exceptions:
 *   - In the editor profile and batch mode in general,
 *     the ES command may be used freely.
 *   - In the ED hook macro (register \(lqED\(rq),
 *     when a file is added to the ring, most destructive
 *     operations can be performed since rubbing out the
 *     EB command responsible for the hook execution also
 *     removes the buffer from the ring again.
 *   - As part of function key macros that immediately
 *     terminate the command line.
 */
TECO_DEFINE_STATE_EXPECTSTRING(teco_state_scintilla_symbols,
	.process_edit_cmd_cb = (teco_state_process_edit_cmd_cb_t)teco_state_scintilla_symbols_process_edit_cmd,
	.expectstring.last = FALSE
);

static teco_state_t *
teco_state_scintilla_lparam_done(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	if (ctx->mode > TECO_MODE_NORMAL)
		return &teco_state_start;

	sptr_t lParam = 0;

#ifdef HAVE_LEXILLA
	if (ctx->scintilla.iMessage == SCI_SETILEXER) {
		if (teco_string_contains(str, '\0')) {
			g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
			                    "Lexer name must not contain null-byte.");
			return NULL;
		}

		const gchar *lexer = str->data ? : "";
		lParam = (sptr_t)CreateLexer(lexer);
		if (!lParam) {
			g_set_error(error, TECO_ERROR, TECO_ERROR_FAILED,
			            "Lexilla lexer \"%s\" not found.", lexer);
			return NULL;
		}
	} else
#endif
	if (str->len > 0) {
		/*
		 * NOTE: There may even be messages that read strings
		 * with embedded nulls.
		 */
		lParam = (sptr_t)str->data;
	} else {
		teco_int_t v;
		if (!teco_expressions_pop_num_calc(&v, 0, error))
			return NULL;
		lParam = v;
	}

	teco_expressions_push(teco_interface_ssm(ctx->scintilla.iMessage,
	                                         ctx->scintilla.wParam, lParam));

	return &teco_state_start;
}

TECO_DEFINE_STATE_EXPECTSTRING(teco_state_scintilla_lparam);
