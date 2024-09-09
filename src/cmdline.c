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

#include <string.h>
#include <signal.h>

#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif
#ifdef HAVE_MALLOC_NP_H
#include <malloc_np.h>
#endif

#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>

#include "sciteco.h"
#include "string-utils.h"
#include "file-utils.h"
#include "interface.h"
#include "view.h"
#include "expressions.h"
#include "parser.h"
#include "core-commands.h"
#include "qreg-commands.h"
#include "qreg.h"
#include "ring.h"
#include "goto.h"
#include "help.h"
#include "undo.h"
#include "symbols.h"
#include "spawn.h"
#include "eol.h"
#include "error.h"
#include "qreg.h"
#include "cmdline.h"

#if defined(HAVE_MALLOC_TRIM) && !defined(HAVE_DECL_MALLOC_TRIM)
int malloc_trim(size_t pad);
#endif

#define TECO_DEFAULT_BREAK_CHARS " \t\v\r\n\f<>,;@"

teco_cmdline_t teco_cmdline = {};

/*
 * FIXME: Should this be here?
 * Should perhaps rather be in teco_machine_main_t or teco_cmdline_t.
 */
gboolean teco_quit_requested = FALSE;

/** Last terminated command line */
static teco_string_t teco_last_cmdline = {NULL, 0};

/**
 * Insert string into command line and execute
 * it immediately.
 * It already handles command line replacement (TECO_ERROR_CMDLINE).
 *
 * @param data String to insert.
 * @param len Length of string to insert.
 * @param error A GError.
 * @return FALSE to throw a GError
 */
gboolean
teco_cmdline_insert(const gchar *data, gsize len, GError **error)
{
	const teco_string_t src = {(gchar *)data, len};
	teco_string_t old_cmdline = {NULL, 0};
	guint repl_pc = 0;

	teco_cmdline.machine.macro_pc = teco_cmdline.pc = teco_cmdline.effective_len;

	if (len <= teco_cmdline.str.len - teco_cmdline.effective_len &&
	    !teco_string_cmp(&src, teco_cmdline.str.data + teco_cmdline.effective_len, len)) {
		teco_cmdline.effective_len += len;
	} else {
		if (teco_cmdline.effective_len < teco_cmdline.str.len)
			/*
			 * Automatically disable immediate editing modifier.
			 * FIXME: Should we show a message as when pressing ^G?
			 */
			teco_cmdline.modifier_enabled = FALSE;

		teco_cmdline.str.len = teco_cmdline.effective_len;
		teco_string_append(&teco_cmdline.str, data, len);
		teco_cmdline.effective_len = teco_cmdline.str.len;
	}

	/*
	 * Parse/execute characters, one at a time so
	 * undo tokens get emitted for the corresponding characters.
	 *
	 * FIXME: The inner loop should be factored out.
	 */
	while (teco_cmdline.pc < teco_cmdline.effective_len) {
		g_autoptr(GError) tmp_error = NULL;

		if (!teco_machine_main_step(&teco_cmdline.machine, teco_cmdline.str.data,
		                            teco_cmdline.pc+1, &tmp_error)) {
			if (g_error_matches(tmp_error, TECO_ERROR, TECO_ERROR_CMDLINE)) {
				/*
				 * Result of command line replacement (}):
				 * Exchange command lines, avoiding deep copying
				 */
				teco_qreg_t *cmdline_reg = teco_qreg_table_find(&teco_qreg_table_globals, "\e", 1);
				teco_string_t new_cmdline;

				if (!cmdline_reg->vtable->get_string(cmdline_reg, &new_cmdline.data, &new_cmdline.len,
				                                     NULL, error))
					return FALSE;

				/*
				 * Search for first differing character in old and
				 * new command line. This avoids unnecessary rubouts
				 * and insertions when the command line is updated.
				 */
				teco_cmdline.pc = teco_string_diff(&teco_cmdline.str, new_cmdline.data, new_cmdline.len);

				teco_undo_pop(teco_cmdline.pc);

				g_assert(old_cmdline.len == 0);
				old_cmdline = teco_cmdline.str;
				teco_cmdline.str = new_cmdline;
				teco_cmdline.effective_len = new_cmdline.len;
				teco_cmdline.machine.macro_pc = repl_pc = teco_cmdline.pc;

				continue;
			}

			if (tmp_error->domain != TECO_ERROR || tmp_error->code < TECO_ERROR_CMDLINE) {
				teco_error_add_frame_toplevel();
				teco_error_display_short(tmp_error);

				if (old_cmdline.len > 0) {
					/*
					 * Error during command-line replacement.
					 * Replay previous command-line.
					 * This avoids deep copying.
					 */
					teco_undo_pop(repl_pc);

					teco_string_clear(&teco_cmdline.str);
					teco_cmdline.str = old_cmdline;
					teco_cmdline.machine.macro_pc = teco_cmdline.pc = repl_pc;

					/* rubout cmdline replacement command */
					teco_cmdline.effective_len--;
					continue;
				}
			}

			/* error is handled in teco_cmdline_keypress_c() */
			g_propagate_error(error, g_steal_pointer(&tmp_error));
			return FALSE;
		}

		teco_cmdline.pc++;
	}

	return TRUE;
}

gboolean
teco_cmdline_rubin(GError **error)
{
	if (!teco_cmdline.str.len)
		return TRUE;

	const gchar *start, *end, *next;
	start = teco_cmdline.str.data+teco_cmdline.effective_len;
	end = teco_cmdline.str.data+teco_cmdline.str.len;
	next = g_utf8_find_next_char(start, end) ? : end;
	return teco_cmdline_insert(start, next-start, error);
}

gboolean
teco_cmdline_keypress_c(gchar key, GError **error)
{
	teco_machine_t *machine = &teco_cmdline.machine.parent;
	g_autoptr(GError) tmp_error = NULL;

	/*
	 * Cleanup messages,etc...
	 */
	teco_interface_msg_clear();

	/*
	 * Process immediate editing commands, inserting
	 * characters as necessary into the command line.
	 */
	if (!machine->current->process_edit_cmd_cb(machine, NULL, key, &tmp_error)) {
		if (g_error_matches(tmp_error, TECO_ERROR, TECO_ERROR_RETURN)) {
			/*
			 * Return from top-level macro, results
			 * in command line termination.
			 * The return "arguments" are currently
			 * ignored.
			 */
			g_assert(machine->current == &teco_state_start);

			teco_interface_popup_clear();

			if (teco_quit_requested) {
				/* cought by user interface */
				g_set_error_literal(error, TECO_ERROR, TECO_ERROR_QUIT, "");
				return FALSE;
			}

			teco_undo_clear();
			/* also empties all Scintilla undo buffers */
			teco_ring_set_scintilla_undo(TRUE);
			teco_view_set_scintilla_undo(teco_qreg_view, TRUE);
			/*
			 * FIXME: Reset main machine?
			 */
			teco_goto_table_clear(&teco_cmdline.machine.goto_table);
			teco_expressions_clear();
			g_array_remove_range(teco_loop_stack, 0, teco_loop_stack->len);

			teco_string_clear(&teco_last_cmdline);
			teco_last_cmdline = teco_cmdline.str;
			memset(&teco_cmdline.str, 0, sizeof(teco_cmdline.str));
			teco_cmdline.effective_len = 0;
		} else {
			/*
			 * NOTE: Error message already displayed in
			 * teco_cmdline_insert().
			 *
			 * Undo tokens may have been emitted
			 * (or had to be) before the exception
			 * is thrown. They must be executed so
			 * as if the character had never been
			 * inserted.
			 */
			teco_undo_pop(teco_cmdline.pc);
			teco_cmdline.effective_len = teco_cmdline.pc;
			/* program counter could be messed up */
			teco_cmdline.machine.macro_pc = teco_cmdline.effective_len;
		}

#ifdef HAVE_MALLOC_TRIM
		/*
		 * Undo stacks can grow very large - sometimes large enough to
		 * make the system swap and become unresponsive.
		 * This shrinks the program break after lots of memory has
		 * been freed, reducing the virtual memory size and aiding
		 * in recovering from swapping issues.
		 *
		 * This is particularily important with some memory limiting backends
		 * after hitting the memory limit* as otherwise the program's resident
		 * size won't shrink and it would be impossible to recover.
		 */
		if (g_error_matches(tmp_error, TECO_ERROR, TECO_ERROR_RETURN) ||
		    g_error_matches(tmp_error, TECO_ERROR, TECO_ERROR_MEMLIMIT))
			malloc_trim(0);
#endif
	}

	/*
	 * Echo command line
	 */
	teco_interface_cmdline_update(&teco_cmdline);
	return TRUE;
}

gboolean
teco_cmdline_fnmacro(const gchar *name, GError **error)
{
	g_assert(name != NULL);

	/*
	 * NOTE: It should be safe to allocate on the stack since
	 * there are only a limited number of possible function key macros.
	 */
	gchar macro_name[1 + strlen(name)];
	macro_name[0] = TECO_CTL_KEY('F');
	memcpy(macro_name+1, name, sizeof(macro_name)-1);

	teco_qreg_t *macro_reg;

	if (teco_ed & TECO_ED_FNKEYS &&
	    (macro_reg = teco_qreg_table_find(&teco_qreg_table_globals, macro_name, sizeof(macro_name)))) {
		teco_int_t macro_mask;
		if (!macro_reg->vtable->get_integer(macro_reg, &macro_mask, error))
			return FALSE;

		if (macro_mask & teco_cmdline.machine.parent.current->fnmacro_mask)
			return TRUE;

		g_auto(teco_string_t) macro_str = {NULL, 0};
		return macro_reg->vtable->get_string(macro_reg, &macro_str.data, &macro_str.len, NULL, error) &&
		       teco_cmdline_keypress(macro_str.data, macro_str.len, error);
	}

	/*
	 * Most function key macros have no default action,
	 * except "CLOSE" which quits the application
	 * (this may loose unsaved data but is better than
	 * not doing anything if the user closes the window).
	 * NOTE: Doing the check here is less efficient than
	 * doing it in the UI implementations, but defines
	 * the default actions centrally.
	 * Also, fnmacros are only handled after key presses.
	 */
	if (!strcmp(name, "CLOSE")) {
		g_set_error_literal(error, TECO_ERROR, TECO_ERROR_QUIT, "");
		return FALSE;
	}

	return TRUE;
}

void
teco_cmdline_rubout(void)
{
	const gchar *p;
	p = g_utf8_find_prev_char(teco_cmdline.str.data,
	                          teco_cmdline.str.data+teco_cmdline.effective_len);
	if (p) {
		teco_cmdline.effective_len = p - teco_cmdline.str.data;
		teco_undo_pop(teco_cmdline.effective_len);
	}
}

static void TECO_DEBUG_CLEANUP
teco_cmdline_cleanup(void)
{
	teco_machine_main_clear(&teco_cmdline.machine);
	teco_string_clear(&teco_cmdline.str);
	teco_string_clear(&teco_last_cmdline);
}

/*
 * Commandline key processing.
 *
 * These are all the implementations of teco_state_process_edit_cmd_cb_t.
 * It makes sense to use state callbacks for key processing, as it is
 * largely state-dependant; but it defines interactive-mode-only
 * behaviour which can be kept isolated from the rest of the states'
 * implementation.
 */

gboolean
teco_state_process_edit_cmd(teco_machine_t *ctx, teco_machine_t *parent_ctx, gchar key, GError **error)
{
	switch (key) {
	case '\n': /* insert EOL sequence */
		teco_interface_popup_clear();

		if (teco_ed & TECO_ED_AUTOEOL) {
			if (!teco_cmdline_insert("\n", 1, error))
				return FALSE;
		} else {
			const gchar *eol = teco_eol_get_seq(teco_interface_ssm(SCI_GETEOLMODE, 0, 0));
			if (!teco_cmdline_insert(eol, strlen(eol), error))
				return FALSE;
		}
		return TRUE;

	case TECO_CTL_KEY('G'): /* toggle immediate editing modifier */
		teco_interface_popup_clear();

		teco_cmdline.modifier_enabled = !teco_cmdline.modifier_enabled;
		teco_interface_msg(TECO_MSG_INFO,
		                   "Immediate editing modifier is now %s.",
		                   teco_cmdline.modifier_enabled ? "enabled" : "disabled");
		return TRUE;

	case TECO_CTL_KEY('H'): /* rubout/reinsert character */
		teco_interface_popup_clear();

		if (teco_cmdline.modifier_enabled) {
			/* re-insert character */
			if (!teco_cmdline_rubin(error))
				return FALSE;
		} else {
			/* rubout character */
			teco_cmdline_rubout();
		}
		return TRUE;

	case TECO_CTL_KEY('W'): /* rubout/reinsert command */
		teco_interface_popup_clear();

		if (teco_cmdline.modifier_enabled) {
			/* reinsert command */
			do {
				if (!teco_cmdline_rubin(error))
					return FALSE;
			} while (!ctx->current->is_start &&
			         teco_cmdline.effective_len < teco_cmdline.str.len);
		} else {
			/* rubout command */
			do
				teco_cmdline_rubout();
			while (!ctx->current->is_start);
		}
		return TRUE;

#if !defined(INTERFACE_GTK) && defined(SIGTSTP)
	case TECO_CTL_KEY('Z'):
		/*
		 * <CTL/Z> does not automatically raise signal if handling of
		 * special characters temporarily disabled in terminal
		 * (Curses), or the actual input is detached from terminal (GTK+).
		 * This does NOT change the state of the popup window.
		 */
		raise(SIGTSTP);
		return TRUE;
#endif
	}

	teco_interface_popup_clear();
	return teco_cmdline_insert(&key, sizeof(key), error);
}

gboolean
teco_state_caseinsensitive_process_edit_cmd(teco_machine_t *ctx, teco_machine_t *parent_ctx, gchar key, GError **error)
{
	if (teco_ed & TECO_ED_AUTOCASEFOLD)
		/* will not modify non-letter keys */
		key = g_ascii_islower(key) ? g_ascii_toupper(key)
		                           : g_ascii_tolower(key);

	return teco_state_process_edit_cmd(ctx, parent_ctx, key, error);
}

gboolean
teco_state_stringbuilding_start_process_edit_cmd(teco_machine_stringbuilding_t *ctx, teco_machine_t *parent_ctx,
                                                 gchar key, GError **error)
{
	teco_state_t *current = ctx->parent.current;

	switch (key) {
	case TECO_CTL_KEY('W'): { /* rubout/reinsert word */
		teco_interface_popup_clear();

		g_auto(teco_string_t) wchars;
		wchars.len = teco_interface_ssm(SCI_GETWORDCHARS, 0, 0);
		wchars.data = g_malloc(wchars.len + 1);
		teco_interface_ssm(SCI_GETWORDCHARS, 0, (sptr_t)wchars.data);
		wchars.data[wchars.len] = '\0';

		if (teco_cmdline.modifier_enabled) {
			/* reinsert word chars */
			while (ctx->parent.current == current &&
			       teco_cmdline.effective_len < teco_cmdline.str.len &&
			       teco_string_contains(&wchars, teco_cmdline.str.data[teco_cmdline.effective_len]))
				if (!teco_cmdline_rubin(error))
					return FALSE;

			/* reinsert non-word chars */
			while (ctx->parent.current == current &&
			       teco_cmdline.effective_len < teco_cmdline.str.len &&
			       !teco_string_contains(&wchars, teco_cmdline.str.data[teco_cmdline.effective_len]))
				if (!teco_cmdline_rubin(error))
					return FALSE;

			return TRUE;
		}

		/*
		 * FIXME: In parse-only mode (ctx->result == NULL), we only
		 * get the default behaviour of teco_state_process_edit_cmd().
		 * This may not be a real-life issue serious enough to maintain
		 * a result string even in parse-only mode.
		 *
		 * FIXME: Does not properly rubout string-building commands at the
		 * start of the string argument -- ctx->result->len is not
		 * a valid indicator of argument emptyness.
		 * Since it chains to teco_state_process_edit_cmd() we will instead
		 * rubout the entire command.
		 */
		if (ctx->result && ctx->result->len > 0) {
			gboolean is_wordchar = teco_string_contains(&wchars, teco_cmdline.str.data[teco_cmdline.effective_len-1]);
			teco_cmdline_rubout();
			if (ctx->parent.current != current) {
				/* rub out string building command */
				while (ctx->result->len > 0 && ctx->parent.current != current)
					teco_cmdline_rubout();
				return TRUE;
			}

			/*
			 * rubout non-word chars
			 * FIXME: This might rub out part of string building commands, e.g. "EQ[A]    ^W"
			 */
			if (!is_wordchar) {
				while (ctx->result->len > 0 &&
				       !teco_string_contains(&wchars, teco_cmdline.str.data[teco_cmdline.effective_len-1]))
					teco_cmdline_rubout();
			}

			/* rubout word chars */
			while (ctx->result->len > 0 &&
			       teco_string_contains(&wchars, teco_cmdline.str.data[teco_cmdline.effective_len-1]))
				teco_cmdline_rubout();

			return TRUE;
		}

		/*
		 * Otherwise, the entire string command will be rubbed out.
		 */
		break;
	}

	case TECO_CTL_KEY('U'): /* rubout/reinsert entire string */
		teco_interface_popup_clear();

		if (teco_cmdline.modifier_enabled) {
			/* reinsert string */
			while (ctx->parent.current == current &&
			       teco_cmdline.effective_len < teco_cmdline.str.len)
				if (!teco_cmdline_rubin(error))
					return FALSE;

			return TRUE;
		}

		/*
		 * FIXME: In parse only mode (ctx->result == NULL),
		 * this will chain to teco_state_process_edit_cmd() and rubout
		 * only a single character.
		 */
		if (ctx->result) {
			/* rubout string */
			while (ctx->result->len > 0)
				teco_cmdline_rubout();
			return TRUE;
		}

		break;

	case '\t': { /* autocomplete file name */
		/*
		 * FIXME: Does not autocomplete in parse-only mode (ctx->result == NULL).
		 */
		if (!teco_cmdline.modifier_enabled || !ctx->result)
			break;

		/*
		 * TODO: In insertion commands, we can autocomplete
		 * the string at the buffer cursor.
		 */
		if (teco_interface_popup_is_shown()) {
			/* cycle through popup pages */
			teco_interface_popup_show();
			return TRUE;
		}

		const gchar *filename = teco_string_last_occurrence(ctx->result,
		                                                    TECO_DEFAULT_BREAK_CHARS);
		g_auto(teco_string_t) new_chars, new_chars_escaped;
		gboolean unambiguous = teco_file_auto_complete(filename, G_FILE_TEST_EXISTS, &new_chars);
		teco_machine_stringbuilding_escape(ctx, new_chars.data, new_chars.len, &new_chars_escaped);
		if (unambiguous)
			teco_string_append_c(&new_chars_escaped, ' ');

		if (!teco_cmdline_insert(new_chars_escaped.data, new_chars_escaped.len, error))
			return FALSE;

		/* may be reset if there was a rubbed out command line */
		teco_cmdline.modifier_enabled = TRUE;

		return TRUE;
	}
	}

	/*
	 * Chaining to the parent (embedding) state machine's handler
	 * makes sure that ^W at the beginning of the string argument
	 * rubs out the entire string command.
	 */
	return teco_state_process_edit_cmd(parent_ctx, NULL, key, error);
}

gboolean
teco_state_stringbuilding_qreg_process_edit_cmd(teco_machine_stringbuilding_t *ctx, teco_machine_t *parent_ctx,
                                                gchar chr, GError **error)
{
	g_assert(ctx->machine_qregspec != NULL);
	/* We downcast since teco_machine_qregspec_t is private in qreg.c */
	teco_machine_t *machine = (teco_machine_t *)ctx->machine_qregspec;
	return machine->current->process_edit_cmd_cb(machine, &ctx->parent, chr, error);
}

gboolean
teco_state_expectstring_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx, gchar key, GError **error)
{
	teco_machine_stringbuilding_t *stringbuilding_ctx = &ctx->expectstring.machine;
	teco_state_t *stringbuilding_current = stringbuilding_ctx->parent.current;
	return stringbuilding_current->process_edit_cmd_cb(&stringbuilding_ctx->parent, &ctx->parent, key, error);
}

gboolean
teco_state_insert_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx, gchar key, GError **error)
{
	teco_machine_stringbuilding_t *stringbuilding_ctx = &ctx->expectstring.machine;
	teco_state_t *stringbuilding_current = stringbuilding_ctx->parent.current;

	/*
	 * NOTE: We don't just define teco_state_stringbuilding_start_process_edit_cmd(),
	 * as it would be hard to subclass/overwrite for different main machine states.
	 */
	if (!stringbuilding_current->is_start)
		return stringbuilding_current->process_edit_cmd_cb(&stringbuilding_ctx->parent, &ctx->parent, key, error);

	switch (key) {
	case '\t': { /* insert <TAB> indention */
		if (teco_cmdline.modifier_enabled || teco_interface_ssm(SCI_GETUSETABS, 0, 0))
			break;

		teco_interface_popup_clear();

		/* insert soft tabs */
		gint spaces = teco_interface_ssm(SCI_GETTABWIDTH, 0, 0);
		gint pos = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);
		spaces -= teco_interface_ssm(SCI_GETCOLUMN, pos, 0) % spaces;

		while (spaces--)
			if (!teco_cmdline_insert(" ", 1, error))
				return FALSE;

		return TRUE;
	}
	}

	return stringbuilding_current->process_edit_cmd_cb(&stringbuilding_ctx->parent, &ctx->parent, key, error);
}

gboolean
teco_state_expectfile_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx, gchar key, GError **error)
{
	teco_machine_stringbuilding_t *stringbuilding_ctx = &ctx->expectstring.machine;
	teco_state_t *stringbuilding_current = stringbuilding_ctx->parent.current;

	/*
	 * NOTE: We don't just define teco_state_stringbuilding_start_process_edit_cmd(),
	 * as it would be hard to subclass/overwrite for different main machine states.
	 */
	if (!stringbuilding_current->is_start)
		return stringbuilding_current->process_edit_cmd_cb(&stringbuilding_ctx->parent, &ctx->parent, key, error);

	switch (key) {
	case TECO_CTL_KEY('W'): /* rubout/reinsert file names including directories */
		teco_interface_popup_clear();

		if (teco_cmdline.modifier_enabled) {
			/* reinsert one level of file name */
			while (stringbuilding_ctx->parent.current == stringbuilding_current &&
			       teco_cmdline.effective_len < teco_cmdline.str.len &&
			       !G_IS_DIR_SEPARATOR(teco_cmdline.str.data[teco_cmdline.effective_len]))
				if (!teco_cmdline_rubin(error))
					return FALSE;

			/* reinsert final directory separator */
			if (stringbuilding_ctx->parent.current == stringbuilding_current &&
			    teco_cmdline.effective_len < teco_cmdline.str.len &&
			    G_IS_DIR_SEPARATOR(teco_cmdline.str.data[teco_cmdline.effective_len]) &&
			    !teco_cmdline_rubin(error))
				return FALSE;

			return TRUE;
		}

		if (ctx->expectstring.string.len > 0) {
			/* rubout directory separator */
			if (G_IS_DIR_SEPARATOR(teco_cmdline.str.data[teco_cmdline.effective_len-1]))
				teco_cmdline_rubout();

			/* rubout one level of file name */
			while (ctx->expectstring.string.len > 0 &&
			       !G_IS_DIR_SEPARATOR(teco_cmdline.str.data[teco_cmdline.effective_len-1]))
				teco_cmdline_rubout();

			return TRUE;
		}

		/*
		 * Rub out entire command instead of rubbing out nothing.
		 */
		break;

	case '\t': { /* autocomplete file name */
		if (teco_cmdline.modifier_enabled)
			break;

		if (teco_interface_popup_is_shown()) {
			/* cycle through popup pages */
			teco_interface_popup_show();
			return TRUE;
		}

		if (teco_string_contains(&ctx->expectstring.string, '\0'))
			/* null-byte not allowed in file names */
			return TRUE;

		g_auto(teco_string_t) new_chars, new_chars_escaped;
		gboolean unambiguous = teco_file_auto_complete(ctx->expectstring.string.data, G_FILE_TEST_EXISTS, &new_chars);
		teco_machine_stringbuilding_escape(stringbuilding_ctx, new_chars.data, new_chars.len, &new_chars_escaped);
		if (unambiguous && ctx->expectstring.nesting == 1)
			teco_string_append_c(&new_chars_escaped,
			                     ctx->expectstring.machine.escape_char == '{' ? '}' : ctx->expectstring.machine.escape_char);

		return teco_cmdline_insert(new_chars_escaped.data, new_chars_escaped.len, error);
	}
	}

	return stringbuilding_current->process_edit_cmd_cb(&stringbuilding_ctx->parent, &ctx->parent, key, error);
}

gboolean
teco_state_expectdir_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx, gchar key, GError **error)
{
	teco_machine_stringbuilding_t *stringbuilding_ctx = &ctx->expectstring.machine;
	teco_state_t *stringbuilding_current = stringbuilding_ctx->parent.current;

	/*
	 * NOTE: We don't just define teco_state_stringbuilding_start_process_edit_cmd(),
	 * as it would be hard to subclass/overwrite for different main machine states.
	 */
	if (!stringbuilding_current->is_start)
		return stringbuilding_current->process_edit_cmd_cb(&stringbuilding_ctx->parent, &ctx->parent, key, error);

	switch (key) {
	case '\t': { /* autocomplete directory */
		if (teco_cmdline.modifier_enabled)
			break;

		if (teco_interface_popup_is_shown()) {
			/* cycle through popup pages */
			teco_interface_popup_show();
			return TRUE;
		}

		if (teco_string_contains(&ctx->expectstring.string, '\0'))
			/* null-byte not allowed in file names */
			return TRUE;

		/*
		 * FIXME: We might terminate the command in case of leaf directories.
		 */
		g_auto(teco_string_t) new_chars, new_chars_escaped;
		teco_file_auto_complete(ctx->expectstring.string.data, G_FILE_TEST_IS_DIR, &new_chars);
		teco_machine_stringbuilding_escape(stringbuilding_ctx, new_chars.data, new_chars.len, &new_chars_escaped);

		return teco_cmdline_insert(new_chars_escaped.data, new_chars_escaped.len, error);
	}
	}

	return stringbuilding_current->process_edit_cmd_cb(&stringbuilding_ctx->parent, &ctx->parent, key, error);
}

gboolean
teco_state_expectqreg_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx, gchar key, GError **error)
{
	g_assert(ctx->expectqreg != NULL);
	/*
	 * NOTE: teco_machine_qregspec_t is private, so we downcast to teco_machine_t.
	 * Otherwise, we'd have to move this callback into qreg.c.
	 */
	teco_state_t *expectqreg_current = ((teco_machine_t *)ctx->expectqreg)->current;
	return expectqreg_current->process_edit_cmd_cb((teco_machine_t *)ctx->expectqreg, &ctx->parent, key, error);
}

gboolean
teco_state_qregspec_process_edit_cmd(teco_machine_qregspec_t *ctx, teco_machine_t *parent_ctx, gchar key, GError **error)
{
	switch (key) {
	case '\t': { /* autocomplete Q-Register name */
		if (teco_cmdline.modifier_enabled)
			break;

		if (teco_interface_popup_is_shown()) {
			/* cycle through popup pages */
			teco_interface_popup_show();
			return TRUE;
		}

		/*
		 * NOTE: This is only for short Q-Register specifications,
		 * so there is no escaping.
		 */
		g_auto(teco_string_t) new_chars;
		teco_machine_qregspec_auto_complete(ctx, &new_chars);

		return new_chars.len ? teco_cmdline_insert(new_chars.data, new_chars.len, error) : TRUE;
	}
	}

	/*
	 * We chain to the parent (embedding) state machine's handler
	 * since rubout could otherwise rubout the command, invalidating
	 * the state machine. In particular ^W would crash.
	 * This also makes sure that commands like <EQ> are completely
	 * rub out via ^W.
	 */
	return teco_state_process_edit_cmd(parent_ctx, NULL, key, error);
}

gboolean
teco_state_qregspec_string_process_edit_cmd(teco_machine_qregspec_t *ctx, teco_machine_t *parent_ctx, gchar key, GError **error)
{
	teco_machine_stringbuilding_t *stringbuilding_ctx = teco_machine_qregspec_get_stringbuilding(ctx);
	teco_state_t *stringbuilding_current = stringbuilding_ctx->parent.current;

	/*
	 * NOTE: teco_machine_qregspec_t is private, so we downcast to teco_machine_t.
	 * Otherwise, we'd have to move this callback into qreg.c.
	 *
	 * NOTE: We don't just define teco_state_stringbuilding_start_process_edit_cmd(),
	 * as it would be hard to subclass/overwrite for different main machine states.
	 */
	if (!stringbuilding_current->is_start)
		return stringbuilding_current->process_edit_cmd_cb(&stringbuilding_ctx->parent, (teco_machine_t *)ctx, key, error);

	switch (key) {
	case '\t': { /* autocomplete Q-Register name */
		if (teco_cmdline.modifier_enabled)
			break;

		if (teco_interface_popup_is_shown()) {
			/* cycle through popup pages */
			teco_interface_popup_show();
			return TRUE;
		}

		g_auto(teco_string_t) new_chars, new_chars_escaped;
		gboolean unambiguous = teco_machine_qregspec_auto_complete(ctx, &new_chars);
		teco_machine_stringbuilding_escape(stringbuilding_ctx, new_chars.data, new_chars.len, &new_chars_escaped);
		if (unambiguous)
			teco_string_append_c(&new_chars_escaped, ']');

		return new_chars_escaped.len ? teco_cmdline_insert(new_chars_escaped.data, new_chars_escaped.len, error) : TRUE;
	}
	}

	return stringbuilding_current->process_edit_cmd_cb(&stringbuilding_ctx->parent, (teco_machine_t *)ctx, key, error);
}

gboolean
teco_state_execute_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx, gchar key, GError **error)
{
	teco_machine_stringbuilding_t *stringbuilding_ctx = &ctx->expectstring.machine;
	teco_state_t *stringbuilding_current = stringbuilding_ctx->parent.current;

	/*
	 * NOTE: We don't just define teco_state_stringbuilding_start_process_edit_cmd(),
	 * as it would be hard to subclass/overwrite for different main machine states.
	 */
	if (!stringbuilding_current->is_start)
		return stringbuilding_current->process_edit_cmd_cb(&stringbuilding_ctx->parent, &ctx->parent, key, error);

	switch (key) {
	case '\t': { /* autocomplete file name */
		if (teco_cmdline.modifier_enabled)
			break;

		/*
		 * In the EC command, <TAB> completes files just like ^T
		 *
		 * TODO: Implement shell-command completion by iterating
		 * executables in $PATH
		 */
		if (teco_interface_popup_is_shown()) {
			/* cycle through popup pages */
			teco_interface_popup_show();
			return TRUE;
		}

		const gchar *filename = teco_string_last_occurrence(&ctx->expectstring.string,
		                                                    TECO_DEFAULT_BREAK_CHARS);
		g_auto(teco_string_t) new_chars, new_chars_escaped;
		gboolean unambiguous = teco_file_auto_complete(filename, G_FILE_TEST_EXISTS, &new_chars);
		teco_machine_stringbuilding_escape(stringbuilding_ctx, new_chars.data, new_chars.len, &new_chars_escaped);
		if (unambiguous)
			teco_string_append_c(&new_chars_escaped, ' ');

		return teco_cmdline_insert(new_chars_escaped.data, new_chars_escaped.len, error);
	}
	}

	return stringbuilding_current->process_edit_cmd_cb(&stringbuilding_ctx->parent, &ctx->parent, key, error);
}

gboolean
teco_state_scintilla_symbols_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx, gchar key, GError **error)
{
	teco_machine_stringbuilding_t *stringbuilding_ctx = &ctx->expectstring.machine;
	teco_state_t *stringbuilding_current = stringbuilding_ctx->parent.current;

	/*
	 * NOTE: We don't just define teco_state_stringbuilding_start_process_edit_cmd(),
	 * as it would be hard to subclass/overwrite for different main machine states.
	 */
	if (!stringbuilding_current->is_start)
		return stringbuilding_current->process_edit_cmd_cb(&stringbuilding_ctx->parent, &ctx->parent, key, error);

	switch (key) {
	case '\t': { /* autocomplete Scintilla symbol */
		if (teco_cmdline.modifier_enabled)
			break;

		if (teco_interface_popup_is_shown()) {
			/* cycle through popup pages */
			teco_interface_popup_show();
			return TRUE;
		}

		const gchar *symbol = teco_string_last_occurrence(&ctx->expectstring.string, ",");
		teco_symbol_list_t *list = symbol == ctx->expectstring.string.data
						? &teco_symbol_list_scintilla
						: &teco_symbol_list_scilexer;
		g_auto(teco_string_t) new_chars, new_chars_escaped;
		gboolean unambiguous = teco_symbol_list_auto_complete(list, symbol, &new_chars);

		teco_machine_stringbuilding_escape(stringbuilding_ctx, new_chars.data, new_chars.len, &new_chars_escaped);
		/*
		 * FIXME: Does not escape `,`.
		 */
		if (unambiguous)
			teco_string_append_c(&new_chars_escaped, ',');

		return teco_cmdline_insert(new_chars_escaped.data, new_chars_escaped.len, error);
	}
	}

	return stringbuilding_current->process_edit_cmd_cb(&stringbuilding_ctx->parent, &ctx->parent, key, error);
}

gboolean
teco_state_goto_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx, gchar key, GError **error)
{
	teco_machine_stringbuilding_t *stringbuilding_ctx = &ctx->expectstring.machine;
	teco_state_t *stringbuilding_current = stringbuilding_ctx->parent.current;

	/*
	 * NOTE: We don't just define teco_state_stringbuilding_start_process_edit_cmd(),
	 * as it would be hard to subclass/overwrite for different main machine states.
	 */
	if (!stringbuilding_current->is_start)
		return stringbuilding_current->process_edit_cmd_cb(&stringbuilding_ctx->parent, &ctx->parent, key, error);

	switch (key) {
	case '\t': { /* autocomplete goto label */
		if (teco_cmdline.modifier_enabled)
			break;

		if (teco_interface_popup_is_shown()) {
			/* cycle through popup pages */
			teco_interface_popup_show();
			return TRUE;
		}

		teco_string_t label = ctx->expectstring.string;
		gint i = teco_string_rindex(&label, ',');
		if (i >= 0) {
			label.data += i+1;
			label.len -= i+1;
		}

		g_auto(teco_string_t) new_chars, new_chars_escaped;
		gboolean unambiguous = teco_goto_table_auto_complete(&ctx->goto_table, label.data, label.len, &new_chars);
		/*
		 * FIXME: This does not escape `,`. Cannot be escaped via ^Q currently?
		 */
		teco_machine_stringbuilding_escape(stringbuilding_ctx, new_chars.data, new_chars.len, &new_chars_escaped);
		if (unambiguous)
			teco_string_append_c(&new_chars_escaped, ',');

		return new_chars_escaped.len ? teco_cmdline_insert(new_chars_escaped.data, new_chars_escaped.len, error) : TRUE;
	}
	}

	return stringbuilding_current->process_edit_cmd_cb(&stringbuilding_ctx->parent, &ctx->parent, key, error);
}

gboolean
teco_state_help_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx, gchar key, GError **error)
{
	teco_machine_stringbuilding_t *stringbuilding_ctx = &ctx->expectstring.machine;
	teco_state_t *stringbuilding_current = stringbuilding_ctx->parent.current;

	/*
	 * NOTE: We don't just define teco_state_stringbuilding_start_process_edit_cmd(),
	 * as it would be hard to subclass/overwrite for different main machine states.
	 */
	if (!stringbuilding_current->is_start)
		return stringbuilding_current->process_edit_cmd_cb(&stringbuilding_ctx->parent, &ctx->parent, key, error);

	switch (key) {
	case '\t': { /* autocomplete help term */
		if (teco_cmdline.modifier_enabled)
			break;

		if (teco_interface_popup_is_shown()) {
			/* cycle through popup pages */
			teco_interface_popup_show();
			return TRUE;
		}

		if (teco_string_contains(&ctx->expectstring.string, '\0'))
			/* help term must not contain null-byte */
			return TRUE;

		g_auto(teco_string_t) new_chars, new_chars_escaped;
		gboolean unambiguous = teco_help_auto_complete(ctx->expectstring.string.data, &new_chars);
		teco_machine_stringbuilding_escape(stringbuilding_ctx, new_chars.data, new_chars.len, &new_chars_escaped);
		if (unambiguous && ctx->expectstring.nesting == 1)
			teco_string_append_c(&new_chars_escaped,
			                     ctx->expectstring.machine.escape_char == '{' ? '}' : ctx->expectstring.machine.escape_char);

		return new_chars_escaped.len ? teco_cmdline_insert(new_chars_escaped.data, new_chars_escaped.len, error) : TRUE;
	}
	}

	return stringbuilding_current->process_edit_cmd_cb(&stringbuilding_ctx->parent, &ctx->parent, key, error);
}

/*
 * Command states
 */

static teco_state_t *
teco_state_save_cmdline_got_register(teco_machine_main_t *ctx, teco_qreg_t *qreg,
                                     teco_qreg_table_t *table, GError **error)
{
	teco_state_expectqreg_reset(ctx);

	if (ctx->mode != TECO_MODE_NORMAL)
		return &teco_state_start;

	if (!qreg->vtable->undo_set_string(qreg, error) ||
	    !qreg->vtable->set_string(qreg, teco_last_cmdline.data, teco_last_cmdline.len,
	                              teco_default_codepage(), error))
		return NULL;

	return &teco_state_start;
}

/*$ *q
 * *q -- Save last command line
 *
 * Only at the very beginning of a command-line, this command
 * may be used to save the last command line as a string in
 * Q-Register <q>.
 */
TECO_DEFINE_STATE_EXPECTQREG(teco_state_save_cmdline,
	.expectqreg.type = TECO_QREG_OPTIONAL_INIT
);
