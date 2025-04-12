/*
 * Copyright (C) 2012-2025 Robin Haberkorn
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
#include "glob.h"
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
static gboolean
teco_cmdline_insert(const gchar *data, gsize len, GError **error)
{
	const teco_string_t src = {(gchar *)data, len};
	g_auto(teco_string_t) old_cmdline = {NULL, 0};
	gsize repl_pc = 0;

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
					memset(&old_cmdline, 0, sizeof(old_cmdline));
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

static gboolean
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

/**
 * Process key press or expansion of key macro.
 *
 * Should be called only with the results of a single keypress.
 * They are considered an unity and in case of errors, we
 * rubout the entire sequence (unless there was a $$ return in the
 * middle).
 *
 * @param data Key presses in UTF-8.
 * @param len Length of data.
 * @param error A GError.
 * @return FALSE if error was set.
 *   If TRUE was returned, there could still have been an error,
 *   but it has already been handled.
 */
gboolean
teco_cmdline_keypress(const gchar *data, gsize len, GError **error)
{
	const teco_string_t str = {(gchar *)data, len};
	teco_machine_t *machine = &teco_cmdline.machine.parent;

	if (!teco_string_validate_utf8(&str)) {
		g_set_error_literal(error, TECO_ERROR, TECO_ERROR_CODEPOINT,
		                    "Invalid UTF-8 sequence");
		return FALSE;
	}

	/*
	 * Cleanup messages, etc...
	 */
	teco_interface_msg_clear();

	gsize start_pc = teco_cmdline.effective_len;

	for (guint i = 0; i < len; i = g_utf8_next_char(data+i) - data) {
		gunichar chr = g_utf8_get_char(data+i);
		g_autoptr(GError) tmp_error = NULL;

		/*
		 * Process immediate editing commands, inserting
		 * characters as necessary into the command line.
		 */
		if (machine->current->process_edit_cmd_cb(machine, NULL, chr, &tmp_error))
			continue;

		if (!g_error_matches(tmp_error, TECO_ERROR, TECO_ERROR_RETURN)) {
			/*
			 * NOTE: Error message already displayed in
			 * teco_cmdline_insert().
			 *
			 * Undo tokens may have been emitted
			 * (or had to be) before the exception
			 * is thrown. They must be executed so
			 * as if the character had never been
			 * inserted.
			 * Actually we rub out the entire command line
			 * up until the insertion point.
			 */
			teco_undo_pop(start_pc);
			teco_cmdline.effective_len = start_pc;
			/* program counter could be messed up */
			teco_cmdline.machine.macro_pc = teco_cmdline.effective_len;

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
			if (g_error_matches(tmp_error, TECO_ERROR, TECO_ERROR_MEMLIMIT))
				malloc_trim(0);
#endif

			break;
		}

		/*
		 * Return from top-level macro, results
		 * in command line termination.
		 * The return "arguments" are currently
		 * ignored.
		 */
		g_assert(machine->current == &teco_state_start);

		teco_interface_popup_clear();

		if (teco_quit_requested) {
			/* caught by user interface */
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

#ifdef HAVE_MALLOC_TRIM
		/* see above */
		malloc_trim(0);
#endif

		/*
		 * Continue with the other keys,
		 * but we obviously can't rub out beyond the return if any
		 * error occurs later on.
		 */
		start_pc = 0;
	}

	/*
	 * Echo command line
	 */
	teco_interface_cmdline_update(&teco_cmdline);
	return TRUE;
}

teco_keymacro_status_t
teco_cmdline_keymacro(const gchar *name, gssize name_len, GError **error)
{
	g_assert(name != NULL);

	if (name_len < 0)
		name_len = strlen(name);

	/*
	 * NOTE: It should be safe to allocate on the stack since
	 * there are only a limited number of possible function key macros.
	 */
	gchar macro_name[1 + name_len];
	macro_name[0] = TECO_CTL_KEY('K');
	memcpy(macro_name+1, name, name_len);

	teco_qreg_t *macro_reg = teco_qreg_table_find(&teco_qreg_table_globals, macro_name, sizeof(macro_name));
	if (macro_reg) {
		teco_int_t macro_mask;
		if (!macro_reg->vtable->get_integer(macro_reg, &macro_mask, error))
			return TECO_KEYMACRO_ERROR;

		/*
		 * FIXME: This does not work with Q-Register specs embedded into string arguments.
		 * There should be a keymacro_mask_cb() instead.
		 */
		if (!((teco_cmdline.machine.parent.current->keymacro_mask |
		       teco_cmdline.machine.expectstring.machine.parent.current->keymacro_mask) & ~macro_mask))
			return TECO_KEYMACRO_UNDEFINED;

		g_auto(teco_string_t) macro_str = {NULL, 0};
		return macro_reg->vtable->get_string(macro_reg, &macro_str.data, &macro_str.len, NULL, error) &&
		       teco_cmdline_keypress(macro_str.data, macro_str.len, error)
					? TECO_KEYMACRO_SUCCESS : TECO_KEYMACRO_ERROR;
	}

	/*
	 * Most function key macros have no default action,
	 * except "CLOSE" which quits the application
	 * (this may loose unsaved data but is better than
	 * not doing anything if the user closes the window).
	 */
	if (name_len == 5 && !strncmp(name, "CLOSE", name_len)) {
		g_set_error_literal(error, TECO_ERROR, TECO_ERROR_QUIT, "");
		return TECO_KEYMACRO_ERROR;
	}

	return TECO_KEYMACRO_UNDEFINED;
}

static void
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
teco_state_process_edit_cmd(teco_machine_t *ctx, teco_machine_t *parent_ctx, gunichar key, GError **error)
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

	case TECO_CTL_KEY('W'): /* rubout/reinsert construct */
		teco_interface_popup_clear();

		if (teco_cmdline.modifier_enabled) {
			/* reinsert construct */
			do {
				if (!teco_cmdline_rubin(error))
					return FALSE;
			} while (!ctx->current->is_start &&
			         teco_cmdline.effective_len < teco_cmdline.str.len);
		} else {
			/* rubout construct */
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

	gchar buf[6];
	gsize len = g_unichar_to_utf8(key, buf);
	return teco_cmdline_insert(buf, len, error);
}

gboolean
teco_state_caseinsensitive_process_edit_cmd(teco_machine_t *ctx, teco_machine_t *parent_ctx, gunichar key, GError **error)
{
	/*
	 * Auto case folding is for syntactic characters,
	 * so this could be done by working only with a-z and A-Z.
	 * However, it's also not speed critical.
	 */
	if (teco_ed & TECO_ED_AUTOCASEFOLD)
		key = g_unichar_islower(key) ? g_unichar_toupper(key)
		                             : g_unichar_tolower(key);

	return teco_state_process_edit_cmd(ctx, parent_ctx, key, error);
}

gboolean
teco_state_command_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx, gunichar key, GError **error)
{
	switch (key) {
	case TECO_CTL_KEY('W'): /* rubout/reinsert command */
		teco_interface_popup_clear();

		/*
		 * This mimics the behavior of the `Y` command,
		 * so it also rubs out no-op commands.
		 * See also teco_find_words().
		 */
		if (teco_cmdline.modifier_enabled) {
			/* reinsert command */
			/* @ and : are not separate states, but practically belong to the command */
			while (ctx->parent.current->is_start &&
			       teco_cmdline.effective_len < teco_cmdline.str.len &&
			       (teco_cmdline.str.data[teco_cmdline.effective_len] == ':' ||
			        teco_cmdline.str.data[teco_cmdline.effective_len] == '@'))
				if (!teco_cmdline_rubin(error))
					return FALSE;

			do {
				if (!teco_cmdline_rubin(error))
					return FALSE;
			} while (!ctx->parent.current->is_start &&
			         teco_cmdline.effective_len < teco_cmdline.str.len);

			while (ctx->parent.current->is_start &&
			       teco_cmdline.effective_len < teco_cmdline.str.len &&
			       strchr(TECO_NOOPS, teco_cmdline.str.data[teco_cmdline.effective_len]))
				if (!teco_cmdline_rubin(error))
					return FALSE;

			return TRUE;
		}

		/* rubout command */
		while (ctx->parent.current->is_start &&
		       teco_cmdline.effective_len > 0 &&
		       strchr(TECO_NOOPS, teco_cmdline.str.data[teco_cmdline.effective_len-1]))
			teco_cmdline_rubout();

		do
			teco_cmdline_rubout();
		while (!ctx->parent.current->is_start);

		/*
		 * @ and : are not separate states, but practically belong to the command.
		 * We cannot rely on the last character though, since it might
		 * be part of another command.
		 */
		while (ctx->parent.current->is_start &&
		       (ctx->flags.modifier_at || ctx->flags.modifier_colon) &&
		       teco_cmdline.effective_len > 0)
			teco_cmdline_rubout();

		return TRUE;
	}

	return teco_state_caseinsensitive_process_edit_cmd(&ctx->parent, parent_ctx, key, error);
}

gboolean
teco_state_stringbuilding_start_process_edit_cmd(teco_machine_stringbuilding_t *ctx, teco_machine_t *parent_ctx,
                                                 gunichar key, GError **error)
{
	teco_state_t *current = ctx->parent.current;

	switch (key) {
	case TECO_CTL_KEY('W'): { /* rubout/reinsert word */
		teco_interface_popup_clear();

		/*
		 * NOTE: This must be consistent with teco_find_words():
		 * Always delete to the beginning of the previous word.
		 */
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
		 */
		if (ctx->result && ctx->result->len > 0) {
			gboolean is_wordchar = teco_string_contains(&wchars, teco_cmdline.str.data[teco_cmdline.effective_len-1]);
			teco_cmdline_rubout();
			if (ctx->parent.current != current) {
				/* rub out string building command */
				do
					teco_cmdline_rubout();
				while (ctx->parent.current != current);
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
			teco_interface_popup_scroll();
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
	 *
	 * FIXME: This does not rub out modifiers in front of
	 * string commands since this callback could be used in recursively
	 * embedded string building machines as well.
	 */
	return teco_state_process_edit_cmd(parent_ctx, NULL, key, error);
}

gboolean
teco_state_stringbuilding_insert_completion(teco_machine_stringbuilding_t *ctx, const teco_string_t *str, GError **error)
{
	g_auto(teco_string_t) str_escaped;
	teco_machine_stringbuilding_escape(ctx, str->data, str->len, &str_escaped);
	if (!str->len || !G_IS_DIR_SEPARATOR(str->data[str->len-1]))
		teco_string_append_c(&str_escaped, ' ');
	return teco_cmdline_insert(str_escaped.data, str_escaped.len, error);
}

gboolean
teco_state_stringbuilding_escaped_process_edit_cmd(teco_machine_stringbuilding_t *ctx, teco_machine_t *parent_ctx,
                                                   gunichar key, GError **error)
{
	/*
	 * Allow insertion of characters that would otherwise be interpreted as
	 * immediate editing commands after ^Q/^R.
	 */
	switch (key) {
	//case TECO_CTL_KEY('G'):
	case TECO_CTL_KEY('W'):
	case TECO_CTL_KEY('U'):
		teco_interface_popup_clear();

		gchar c = key;
		return teco_cmdline_insert(&c, sizeof(c), error);
	}

	return teco_state_process_edit_cmd(parent_ctx, NULL, key, error);
}

gboolean
teco_state_stringbuilding_qreg_process_edit_cmd(teco_machine_stringbuilding_t *ctx, teco_machine_t *parent_ctx,
                                                gunichar chr, GError **error)
{
	g_assert(ctx->machine_qregspec != NULL);
	/* We downcast since teco_machine_qregspec_t is private in qreg.c */
	teco_machine_t *machine = (teco_machine_t *)ctx->machine_qregspec;
	return machine->current->process_edit_cmd_cb(machine, &ctx->parent, chr, error);
}

gboolean
teco_state_expectstring_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx, gunichar key, GError **error)
{
	teco_machine_stringbuilding_t *stringbuilding_ctx = &ctx->expectstring.machine;
	teco_state_t *stringbuilding_current = stringbuilding_ctx->parent.current;
	return stringbuilding_current->process_edit_cmd_cb(&stringbuilding_ctx->parent, &ctx->parent, key, error);
}

gboolean
teco_state_expectstring_insert_completion(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	teco_machine_stringbuilding_t *stringbuilding_ctx = &ctx->expectstring.machine;
	teco_state_t *stringbuilding_current = stringbuilding_ctx->parent.current;
	return stringbuilding_current->insert_completion_cb(&stringbuilding_ctx->parent, str, error);
}

gboolean
teco_state_insert_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx, gunichar key, GError **error)
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
teco_state_expectfile_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx, gunichar key, GError **error)
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
			teco_interface_popup_scroll();
			return TRUE;
		}

		if (teco_string_contains(&ctx->expectstring.string, '\0'))
			/* null-byte not allowed in file names */
			return TRUE;

		g_auto(teco_string_t) new_chars, new_chars_escaped;
		gboolean unambiguous = teco_file_auto_complete(ctx->expectstring.string.data, G_FILE_TEST_EXISTS, &new_chars);
		teco_machine_stringbuilding_escape(stringbuilding_ctx, new_chars.data, new_chars.len, &new_chars_escaped);
		if (unambiguous && ctx->expectstring.nesting == 1)
			teco_string_append_wc(&new_chars_escaped,
			                      ctx->expectstring.machine.escape_char == '{' ? '}' : ctx->expectstring.machine.escape_char);

		return teco_cmdline_insert(new_chars_escaped.data, new_chars_escaped.len, error);
	}
	}

	return stringbuilding_current->process_edit_cmd_cb(&stringbuilding_ctx->parent, &ctx->parent, key, error);
}

gboolean
teco_state_expectfile_insert_completion(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	teco_machine_stringbuilding_t *stringbuilding_ctx = &ctx->expectstring.machine;

	g_auto(teco_string_t) str_escaped;
	teco_machine_stringbuilding_escape(stringbuilding_ctx, str->data, str->len, &str_escaped);
	if ((!str->len || !G_IS_DIR_SEPARATOR(str->data[str->len-1])) &&
	    ctx->expectstring.nesting == 1)
		teco_string_append_wc(&str_escaped,
		                      ctx->expectstring.machine.escape_char == '{' ? '}' : ctx->expectstring.machine.escape_char);
	return teco_cmdline_insert(str_escaped.data, str_escaped.len, error);
}

gboolean
teco_state_expectglob_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx, gunichar key, GError **error)
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

		if (teco_interface_popup_is_shown()) {
			/* cycle through popup pages */
			teco_interface_popup_scroll();
			return TRUE;
		}

		if (teco_string_contains(&ctx->expectstring.string, '\0'))
			/* null-byte not allowed in file names */
			return TRUE;

		/*
		 * We do not support autocompleting glob patterns.
		 *
		 * FIXME: What if the last autocompletion inserted escaped glob
		 * characters?
		 * Perhaps teco_file_auto_complete() should natively support glob patterns.
		 */
		if (teco_globber_is_pattern(ctx->expectstring.string.data))
			return TRUE;

		g_auto(teco_string_t) new_chars, new_chars_escaped;
		gboolean unambiguous = teco_file_auto_complete(ctx->expectstring.string.data, G_FILE_TEST_EXISTS, &new_chars);
		g_autofree gchar *pattern_escaped = teco_globber_escape_pattern(new_chars.data);
		teco_machine_stringbuilding_escape(stringbuilding_ctx, pattern_escaped, strlen(pattern_escaped), &new_chars_escaped);
		if (unambiguous && ctx->expectstring.nesting == 1)
			teco_string_append_wc(&new_chars_escaped,
			                      ctx->expectstring.machine.escape_char == '{' ? '}' : ctx->expectstring.machine.escape_char);

		return teco_cmdline_insert(new_chars_escaped.data, new_chars_escaped.len, error);
	}
	}

	/* ^W should behave like in commands accepting files */
	return teco_state_expectfile_process_edit_cmd(ctx, parent_ctx, key, error);
}

gboolean
teco_state_expectglob_insert_completion(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	teco_machine_stringbuilding_t *stringbuilding_ctx = &ctx->expectstring.machine;

	g_autofree gchar *pattern_escaped = teco_globber_escape_pattern(str->data);
	g_auto(teco_string_t) str_escaped;
	teco_machine_stringbuilding_escape(stringbuilding_ctx, pattern_escaped, strlen(pattern_escaped), &str_escaped);
	if ((!str->len || !G_IS_DIR_SEPARATOR(str->data[str->len-1])) &&
	    ctx->expectstring.nesting == 1)
		teco_string_append_wc(&str_escaped,
		                      ctx->expectstring.machine.escape_char == '{' ? '}' : ctx->expectstring.machine.escape_char);
	return teco_cmdline_insert(str_escaped.data, str_escaped.len, error);
}

gboolean
teco_state_expectdir_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx, gunichar key, GError **error)
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
			teco_interface_popup_scroll();
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

	/* ^W should behave like in commands accepting files */
	return teco_state_expectfile_process_edit_cmd(ctx, parent_ctx, key, error);
}

gboolean
teco_state_expectdir_insert_completion(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	teco_machine_stringbuilding_t *stringbuilding_ctx = &ctx->expectstring.machine;

	/*
	 * FIXME: We might terminate the command in case of leaf directories.
	 */
	g_auto(teco_string_t) str_escaped;
	teco_machine_stringbuilding_escape(stringbuilding_ctx, str->data, str->len, &str_escaped);
	return teco_cmdline_insert(str_escaped.data, str_escaped.len, error);
}

gboolean
teco_state_expectqreg_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx, gunichar key, GError **error)
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
teco_state_expectqreg_insert_completion(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	g_assert(ctx->expectqreg != NULL);
	/*
	 * NOTE: teco_machine_qregspec_t is private, so we downcast to teco_machine_t.
	 * Otherwise, we'd have to move this callback into qreg.c.
	 */
	teco_state_t *expectqreg_current = ((teco_machine_t *)ctx->expectqreg)->current;
	return expectqreg_current->insert_completion_cb((teco_machine_t *)ctx->expectqreg, str, error);
}

gboolean
teco_state_qregspec_process_edit_cmd(teco_machine_qregspec_t *ctx, teco_machine_t *parent_ctx, gunichar key, GError **error)
{
	switch (key) {
	case '\t': { /* autocomplete Q-Register name */
		if (teco_cmdline.modifier_enabled)
			break;

		if (teco_interface_popup_is_shown()) {
			/* cycle through popup pages */
			teco_interface_popup_scroll();
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
	 *
	 * FIXME: This does not rub out modifiers in front of
	 * Q-Reg commands since this callback could be used in recursively
	 * embedded Q-Reg specification machines as well.
	 */
	return teco_state_process_edit_cmd(parent_ctx, NULL, key, error);
}

gboolean
teco_state_qregspec_insert_completion(teco_machine_qregspec_t *ctx, const teco_string_t *str, GError **error)
{
	return teco_cmdline_insert(str->data, str->len, error);
}

gboolean
teco_state_qregspec_string_process_edit_cmd(teco_machine_qregspec_t *ctx, teco_machine_t *parent_ctx, gunichar key, GError **error)
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
			teco_interface_popup_scroll();
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
teco_state_qregspec_string_insert_completion(teco_machine_qregspec_t *ctx, const teco_string_t *str, GError **error)
{
	teco_machine_stringbuilding_t *stringbuilding_ctx = teco_machine_qregspec_get_stringbuilding(ctx);

	g_auto(teco_string_t) str_escaped;
	teco_machine_stringbuilding_escape(stringbuilding_ctx, str->data, str->len, &str_escaped);
	teco_string_append_c(&str_escaped, ']');
	return teco_cmdline_insert(str_escaped.data, str_escaped.len, error);
}

gboolean
teco_state_execute_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx, gunichar key, GError **error)
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
		 * In the EC command, <TAB> completes files just like ^G<TAB>.
		 *
		 * TODO: Implement shell-command completion by iterating
		 * executables in $PATH
		 */
		if (teco_interface_popup_is_shown()) {
			/* cycle through popup pages */
			teco_interface_popup_scroll();
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
teco_state_scintilla_symbols_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx, gunichar key, GError **error)
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
			teco_interface_popup_scroll();
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
teco_state_scintilla_symbols_insert_completion(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	teco_machine_stringbuilding_t *stringbuilding_ctx = &ctx->expectstring.machine;

	g_auto(teco_string_t) str_escaped;
	teco_machine_stringbuilding_escape(stringbuilding_ctx, str->data, str->len, &str_escaped);
	teco_string_append_c(&str_escaped, ',');
	return teco_cmdline_insert(str_escaped.data, str_escaped.len, error);
}

gboolean
teco_state_goto_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx, gunichar key, GError **error)
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
			teco_interface_popup_scroll();
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
teco_state_goto_insert_completion(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	teco_machine_stringbuilding_t *stringbuilding_ctx = &ctx->expectstring.machine;

	g_auto(teco_string_t) str_escaped;
	teco_machine_stringbuilding_escape(stringbuilding_ctx, str->data, str->len, &str_escaped);
	/*
	 * FIXME: This does not escape `,`. Cannot be escaped via ^Q currently?
	 */
	teco_string_append_c(&str_escaped, ',');
	return teco_cmdline_insert(str_escaped.data, str_escaped.len, error);
}

gboolean
teco_state_help_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx, gunichar key, GError **error)
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
			teco_interface_popup_scroll();
			return TRUE;
		}

		if (teco_string_contains(&ctx->expectstring.string, '\0'))
			/* help term must not contain null-byte */
			return TRUE;

		g_auto(teco_string_t) new_chars, new_chars_escaped;
		gboolean unambiguous = teco_help_auto_complete(ctx->expectstring.string.data, &new_chars);
		teco_machine_stringbuilding_escape(stringbuilding_ctx, new_chars.data, new_chars.len, &new_chars_escaped);
		if (unambiguous && ctx->expectstring.nesting == 1)
			teco_string_append_wc(&new_chars_escaped,
			                      ctx->expectstring.machine.escape_char == '{' ? '}' : ctx->expectstring.machine.escape_char);

		return new_chars_escaped.len ? teco_cmdline_insert(new_chars_escaped.data, new_chars_escaped.len, error) : TRUE;
	}
	}

	return stringbuilding_current->process_edit_cmd_cb(&stringbuilding_ctx->parent, &ctx->parent, key, error);
}

gboolean
teco_state_help_insert_completion(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	teco_machine_stringbuilding_t *stringbuilding_ctx = &ctx->expectstring.machine;

	g_auto(teco_string_t) str_escaped;
	teco_machine_stringbuilding_escape(stringbuilding_ctx, str->data, str->len, &str_escaped);
	if (ctx->expectstring.nesting == 1)
		teco_string_append_wc(&str_escaped,
		                      ctx->expectstring.machine.escape_char == '{' ? '}' : ctx->expectstring.machine.escape_char);
	return teco_cmdline_insert(str_escaped.data, str_escaped.len, error);
}

/*
 * Command states
 */

static teco_state_t *
teco_state_save_cmdline_got_register(teco_machine_main_t *ctx, teco_qreg_t *qreg,
                                     teco_qreg_table_t *table, GError **error)
{
	teco_state_expectqreg_reset(ctx);

	if (ctx->flags.mode != TECO_MODE_NORMAL)
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
