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

#include <time.h>
#include <string.h>
#include <stdio.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "sciteco.h"
#include "string-utils.h"
#include "file-utils.h"
#include "interface.h"
#include "undo.h"
#include "expressions.h"
#include "ring.h"
#include "parser.h"
#include "lexer.h"
#include "symbols.h"
#include "search.h"
#include "spawn.h"
#include "glob.h"
#include "help.h"
#include "cmdline.h"
#include "error.h"
#include "memory.h"
#include "eol.h"
#include "qreg.h"
#include "stdio-commands.h"
#include "qreg-commands.h"
#include "goto-commands.h"
#include "move-commands.h"
#include "core-commands.h"

static teco_state_t *teco_state_control_input(teco_machine_main_t *ctx, gunichar chr, GError **error);
static teco_state_t *teco_state_ctlc_control_input(teco_machine_main_t *ctx, gunichar chr, GError **error);

/**
 * Translate buffer range arguments from the expression stack to
 * a from-position and length in bytes.
 *
 * If only one argument is given, it is interpreted as a number of lines
 * beginning with dot.
 * If two arguments are given, it is interpreted as two buffer positions
 * in glyphs.
 *
 * @param cmd Name of the command
 * @param from_ret Where to store the from-position in bytes
 * @param len_ret Where to store the length of the range in bytes
 * @param error A GError
 * @return FALSE if an error occurred
 *
 * @fixme There are still redundancies with teco_state_start_kill().
 * But it needs to discern between invalid ranges and other errors.
 */
gboolean
teco_get_range_args(const gchar *cmd, gsize *from_ret, gsize *len_ret, GError **error)
{
	gssize from, len; /* in bytes */

	if (!teco_expressions_eval(FALSE, error))
		return FALSE;

	if (teco_expressions_args() <= 1) {
		teco_int_t line;

		if (!teco_expressions_pop_num_calc(&line, teco_num_sign, error))
			return FALSE;

		from = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);
		line += teco_interface_ssm(SCI_LINEFROMPOSITION, from, 0);

		if (!teco_validate_line(line)) {
			teco_error_range_set(error, cmd);
			return FALSE;
		}

		len = teco_interface_ssm(SCI_POSITIONFROMLINE, line, 0) - from;

		if (len < 0) {
			from += len;
			len *= -1;
		}
	} else {
		gssize to = teco_interface_glyphs2bytes(teco_expressions_pop_num(0));
		from = teco_interface_glyphs2bytes(teco_expressions_pop_num(0));
		len = to - from;

		if (len < 0 || from < 0 || to < 0) {
			teco_error_range_set(error, cmd);
			return FALSE;
		}
	}

	*from_ret = from;
	*len_ret = len;
	return TRUE;
}

/*
 * NOTE: This needs some extra code in teco_state_start_input().
 */
static void
teco_state_start_mul(teco_machine_main_t *ctx, GError **error)
{
	teco_expressions_push_calc(TECO_OP_MUL, error);
}

static void
teco_state_start_div(teco_machine_main_t *ctx, GError **error)
{
	teco_expressions_push_calc(TECO_OP_DIV, error);
}

static void
teco_state_start_plus(teco_machine_main_t *ctx, GError **error)
{
	teco_expressions_push_calc(TECO_OP_ADD, error);
}

static void
teco_state_start_minus(teco_machine_main_t *ctx, GError **error)
{
	if (!teco_expressions_args())
		teco_set_num_sign(-teco_num_sign);
	else
		teco_expressions_push_calc(TECO_OP_SUB, error);
}

static void
teco_state_start_and(teco_machine_main_t *ctx, GError **error)
{
	teco_expressions_push_calc(TECO_OP_AND, error);
}

static void
teco_state_start_or(teco_machine_main_t *ctx, GError **error)
{
	teco_expressions_push_calc(TECO_OP_OR, error);
}

static void
teco_state_start_brace_open(teco_machine_main_t *ctx, GError **error)
{
	if (teco_num_sign < 0) {
		teco_set_num_sign(1);
		if (!teco_expressions_eval(FALSE, error))
			return;
		teco_expressions_push(-1);
		if (!teco_expressions_push_calc(TECO_OP_MUL, error))
			return;
	}
	teco_expressions_brace_open();
}

static void
teco_state_start_brace_close(teco_machine_main_t *ctx, GError **error)
{
	teco_expressions_brace_close(error);
}

static void
teco_state_start_comma(teco_machine_main_t *ctx, GError **error)
{
	if (!teco_expressions_eval(FALSE, error))
		return;
	teco_expressions_push_op(TECO_OP_NEW);
}

/*$ "." dot
 * \&. -> dot -- Return buffer position
 *
 * \(lq.\(rq pushes onto the stack, the current
 * position (also called <dot>) of the currently
 * selected buffer or Q-Register.
 *
 * <dot> is also available in Q-Register \(lq:\(rq.
 */
static void
teco_state_start_dot(teco_machine_main_t *ctx, GError **error)
{
	if (!teco_expressions_eval(FALSE, error))
		return;
	sptr_t pos = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);
	teco_expressions_push(teco_interface_bytes2glyphs(pos));
}

/*$ Z size
 * Z -> size -- Return buffer size
 *
 * Pushes onto the stack, the size of the currently selected
 * buffer or Q-Register.
 * This is value is also the buffer position of the document's
 * end.
 */
static void
teco_state_start_zed(teco_machine_main_t *ctx, GError **error)
{
	if (!teco_expressions_eval(FALSE, error))
		return;
	sptr_t pos = teco_interface_ssm(SCI_GETLENGTH, 0, 0);
	teco_expressions_push(teco_interface_bytes2glyphs(pos));
}

/*$ H
 * H -> 0,Z -- Return range for entire buffer
 *
 * Pushes onto the stack the integer 0 (position of buffer
 * beginning) and the current buffer's size.
 * It is thus often equivalent to the expression
 * \(lq0,Z\(rq, or more generally \(lq(0,Z)\(rq.
 */
static void
teco_state_start_range(teco_machine_main_t *ctx, GError **error)
{
	if (!teco_expressions_eval(FALSE, error))
		return;
	teco_expressions_push(0);
	sptr_t pos = teco_interface_ssm(SCI_GETLENGTH, 0, 0);
	teco_expressions_push(teco_interface_bytes2glyphs(pos));
}

/*$ \[rs]
 * n\\ -- Insert or read ASCII numbers
 * \\ -> n
 *
 * Backslash pops a value from the stack, formats it
 * according to the current radix and inserts it in the
 * current buffer or Q-Register at dot.
 * If <n> is omitted (empty stack), it does the reverse -
 * it reads from the current buffer position an integer
 * in the current radix and pushes it onto the stack.
 * Dot is not changed when reading integers.
 *
 * In other words, the command serializes or deserializes
 * integers as ASCII characters.
 */
static void
teco_state_start_backslash(teco_machine_main_t *ctx, GError **error)
{
	if (!teco_expressions_eval(FALSE, error))
		return;

	if (teco_expressions_args()) {
		gchar buffer[TECO_EXPRESSIONS_FORMAT_LEN];
		gchar *str = teco_expressions_format(buffer,
		                                     teco_expressions_pop_num(0),
		                                     ctx->qreg_table_locals->radix);
		g_assert(*str != '\0');
		gsize len = strlen(str);

		sptr_t pos = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);
		teco_undo_int(teco_ranges[0].from) = teco_interface_bytes2glyphs(pos);
		/*
		 * We can assume that `len` is already in glyphs,
		 * i.e. formatted numbers will never use multi-byte/Unicode characters.
		 */
		teco_undo_int(teco_ranges[0].to) = teco_ranges[0].from + len;
		teco_undo_guint(teco_ranges_count) = 1;

		teco_interface_ssm(SCI_BEGINUNDOACTION, 0, 0);
		teco_interface_ssm(SCI_ADDTEXT, len, (sptr_t)str);
		teco_interface_ssm(SCI_ENDUNDOACTION, 0, 0);
		teco_ring_dirtify();

		if (teco_current_doc_must_undo())
			undo__teco_interface_ssm(SCI_UNDO, 0, 0);
	} else {
		teco_qreg_t *qreg = ctx->qreg_table_locals->radix;
		assert(qreg != NULL);
		teco_int_t radix;
		if (!qreg->vtable->get_integer(qreg, &radix, error))
			return;

		uptr_t pos = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);
		gchar c = (gchar)teco_interface_ssm(SCI_GETCHARAT, pos, 0);
		teco_int_t v = 0;
		gint sign = 1;

		if (c == '-') {
			pos++;
			sign = -1;
		}

		for (;;) {
			c = teco_ascii_toupper((gchar)teco_interface_ssm(SCI_GETCHARAT, pos, 0));
			if (c >= '0' && c <= '0' + MIN(radix, 10) - 1)
				v = (v*radix) + (c - '0');
			else if (c >= 'A' &&
				 c <= 'A' + MIN(radix - 10, 26) - 1)
				v = (v*radix) + 10 + (c - 'A');
			else
				break;

			pos++;
		}

		teco_expressions_push(sign * v);
	}
}

/*
 * NOTE: This needs some extra code in teco_state_start_input().
 */
static void
teco_state_start_loop_open(teco_machine_main_t *ctx, GError **error)
{
	teco_loop_context_t lctx;
	if (!teco_expressions_pop_num_calc(&lctx.counter, -1, error))
		return;
	lctx.brace_level = teco_brace_level;
	lctx.pass_through = teco_machine_main_eval_colon(ctx) > 0;

	if (lctx.counter) {
		/*
		 * Non-colon modified, we add implicit
		 * braces, so loop body won't see parameters.
		 * Colon modified, loop starts can be used
		 * to process stack elements which is symmetric
		 * to ":>".
		 */
		if (!lctx.pass_through)
			teco_expressions_brace_open();

		lctx.pc = ctx->macro_pc;
		g_array_append_val(teco_loop_stack, lctx);
		undo__remove_index__teco_loop_stack(teco_loop_stack->len-1);
	} else {
		/* skip to end of loop */
		if (ctx->parent.must_undo)
			teco_undo_flags(ctx->flags);
		ctx->flags.mode = TECO_MODE_PARSE_ONLY_LOOP;
	}
}

/*
 * NOTE: This needs some extra code in teco_state_start_input().
 */
static void
teco_state_start_loop_close(teco_machine_main_t *ctx, GError **error)
{
	if (teco_loop_stack->len <= ctx->loop_stack_fp) {
		g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
		                    "Loop end without corresponding "
		                    "loop start command");
		return;
	}

	teco_loop_context_t *lctx = &g_array_index(teco_loop_stack, teco_loop_context_t,
	                                           teco_loop_stack->len-1);

	/* only non-pass-through loops increase the brace level */
	if (teco_brace_level != lctx->brace_level + !lctx->pass_through) {
		g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
		                    "Brace left open at loop end command");
		return;
	}

	gboolean colon_modified = teco_machine_main_eval_colon(ctx) > 0;

	/*
	 * Colon-modified loop ends can be used to
	 * aggregate values on the stack.
	 * A non-colon modified ">" behaves like ":>"
	 * for pass-through loop starts, though.
	 */
	if (!lctx->pass_through) {
		if (colon_modified) {
			if (!teco_expressions_brace_close(error))
				return;
			if (lctx->counter != 1)
				teco_expressions_brace_open();
		} else if (!teco_expressions_discard_args(error) ||
		           (lctx->counter == 1 && !teco_expressions_brace_close(error))) {
			return;
		}
	}

	if (lctx->counter == 1) {
		/* this was the last loop iteration */
		undo__insert_val__teco_loop_stack(teco_loop_stack->len-1, *lctx);
		g_array_remove_index(teco_loop_stack, teco_loop_stack->len-1);
	} else {
		/*
		 * Repeat loop:
		 * NOTE: One undo token per iteration could
		 * be avoided by saving the original counter
		 * in the teco_loop_context_t.
		 * We do however optimize the case of infinite loops
		 * because the loop counter does not have to be
		 * updated.
		 */
		ctx->macro_pc = lctx->pc;
		if (lctx->counter >= 0) {
			if (ctx->parent.must_undo)
				teco_undo_int(lctx->counter);
			lctx->counter--;
		}
	}
}

/*$ ";" ":;" break
 * [bool]; -- Conditionally break from loop
 * [bool]:;
 *
 * Breaks from the current inner-most loop if <bool>
 * signifies failure (non-negative value).
 * If colon-modified, breaks from the loop if <bool>
 * signifies success (negative value).
 *
 * If the condition code cannot be popped from the stack,
 * the global search register's condition integer
 * is implied instead.
 * This way, you may break on search success/failures
 * without colon-modifying the search command (or at a
 * later point).
 *
 * Executing \(lq;\(rq outside of iterations in the current
 * macro invocation level yields an error. It is thus not
 * possible to let a macro break a caller's loop.
 */
static void
teco_state_start_break(teco_machine_main_t *ctx, GError **error)
{
	if (teco_loop_stack->len <= ctx->loop_stack_fp) {
		g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
		                    "<;> only allowed in loops");
		return;
	}

	teco_qreg_t *reg = teco_qreg_table_find(&teco_qreg_table_globals, "_", 1);
	g_assert(reg != NULL);
	teco_int_t v;
	if (!reg->vtable->get_integer(reg, &v, error))
		return;

	teco_bool_t rc;
	if (!teco_expressions_pop_num_calc(&rc, v, error))
		return;
	if (teco_machine_main_eval_colon(ctx) > 0)
		rc = ~rc;

	if (teco_is_success(rc))
		return;

	teco_loop_context_t lctx = g_array_index(teco_loop_stack, teco_loop_context_t, teco_loop_stack->len-1);
	g_array_remove_index(teco_loop_stack, teco_loop_stack->len-1);

	if (!teco_expressions_discard_args(error))
		return;
	if (!lctx.pass_through &&
	    !teco_expressions_brace_return(lctx.brace_level, 0, error))
		return;

	undo__insert_val__teco_loop_stack(teco_loop_stack->len, lctx);

	/* skip to end of loop */
	if (ctx->parent.must_undo)
		teco_undo_flags(ctx->flags);
	ctx->flags.mode = TECO_MODE_PARSE_ONLY_LOOP;
}

/*$ "{" "}"
 * { -- Edit command line
 * }
 *
 * The opening curly bracket is a powerful command
 * to edit command lines but has very simple semantics.
 * It copies the current commandline into the global
 * command line editing register (called Escape, i.e.
 * ASCII 27) and edits this register.
 * The curly bracket itself is not copied.
 *
 * The command line may then be edited using any
 * \*(ST command or construct.
 * You may switch between the command line editing
 * register and other registers or buffers.
 * The user will then usually reapply (called update)
 * the current command-line.
 *
 * The closing curly bracket will update the current
 * command-line with the contents of the global command
 * line editing register.
 * To do so it merely rubs-out the current command-line
 * up to the first changed character and inserts
 * all characters following from the updated command
 * line into the command stream.
 * To prevent the undesired rubout of the entire
 * command-line, the replacement command ("}") is only
 * allowed when the replacement register currently edited
 * since it will otherwise be usually empty.
 *
 * .B Note:
 *   - Command line editing only works on command lines,
 *     but not arbitrary macros.
 *     It is therefore not available in batch mode and
 *     will yield an error if used.
 *   - Command line editing commands may be safely used
 *     from macro invocations.
 *     Such macros are called command line editing macros.
 *   - A command line update from a macro invocation will
 *     always yield to the outer-most macro level (i.e.
 *     the command line macro).
 *     Code following the update command in the macro
 *     will thus never be executed.
 *   - As a safe-guard against command line trashing due
 *     to erroneous changes at the beginning of command
 *     lines, a backup mechanism is implemented:
 *     If the updated command line yields an error at
 *     any command during the update, the original
 *     command line will be restored with an algorithm
 *     similar to command line updating and the update
 *     command will fail instead.
 *     That way it behaves like any other command that
 *     yields an error:
 *     The character resulting in the update is rejected
 *     by the command line input subsystem.
 *   - In the rare case that an aforementioned command line
 *     backup fails, the commands following the erroneous
 *     character will not be inserted again (will be lost).
 */
static void
teco_state_start_cmdline_push(teco_machine_main_t *ctx, GError **error)
{
	if (!teco_undo_enabled) {
		g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
		                    "Command-line editing only possible in "
		                    "interactive mode");
		return;
	}

	if (!teco_current_doc_undo_edit(error) ||
	    !teco_qreg_table_edit_name(&teco_qreg_table_globals, "\e", 1, error))
		return;

	teco_interface_ssm(SCI_BEGINUNDOACTION, 0, 0);
	teco_interface_ssm(SCI_CLEARALL, 0, 0);
	teco_interface_ssm(SCI_ADDTEXT, teco_cmdline.pc, (sptr_t)teco_cmdline.str.data);
	teco_interface_ssm(SCI_ENDUNDOACTION, 0, 0);

	/*
	 * Must always support undo on global register.
	 * A undo action should always have been generated.
	 */
	undo__teco_interface_ssm(SCI_UNDO, 0, 0);
}

static void
teco_state_start_cmdline_pop(teco_machine_main_t *ctx, GError **error)
{
	if (!teco_undo_enabled) {
		g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
		                    "Command-line editing only possible in "
		                    "interactive mode");
		return;
	}
	if (teco_qreg_current != teco_qreg_table_find(&teco_qreg_table_globals, "\e", 1)) {
		g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
		                    "Command-line replacement only allowed when "
		                    "editing the replacement register");
		return;
	}

	/* replace cmdline in the outer macro environment */
	g_set_error_literal(error, TECO_ERROR, TECO_ERROR_CMDLINE, "");
}

/*$ A
 * [n]A -> code -- Get character code from buffer
 * -A -> code
 *
 * Returns the character <code> of the character
 * <n> relative to dot from the buffer.
 * This can be an ASCII <code> or Unicode codepoint
 * depending on Scintilla's encoding of the current
 * buffer.
 *
 *   - If <n> is 0, return the <code> of the character
 *     pointed to by dot.
 *   - If <n> is 1, return the <code> of the character
 *     immediately after dot.
 *   - If <n> is -1, return the <code> of the character
 *     immediately preceding dot, ecetera.
 *   - If <n> is omitted, the sign prefix is implied.
 *
 * If the position of the queried character is off-page,
 * the command will return -1.
 * If the document is encoded as UTF-8 and there is
 * an invalid byte sequence at the requested position,
 * -2 is returned.
 * Incomplete byte sequences are returned as -3.
 */
static void
teco_state_start_get(teco_machine_main_t *ctx, GError **error)
{
	teco_int_t v;
	if (!teco_expressions_pop_num_calc(&v, teco_num_sign, error))
		return;

	sptr_t pos = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);
	gssize get_pos = teco_interface_glyphs2bytes_relative(pos, v);
	sptr_t len = teco_interface_ssm(SCI_GETLENGTH, 0, 0);

	teco_expressions_push(get_pos < 0 || get_pos == len
				? -1 : teco_interface_get_character(get_pos, len));
}

teco_state_t *
teco_state_start_input(teco_machine_main_t *ctx, gunichar chr, GError **error)
{
	static teco_machine_main_transition_t transitions[] = {
		/*
		 * Simple transitions
		 */
		['$']  = {&teco_state_escape},
		['!']  = {&teco_state_label},
		['O']  = {&teco_state_goto,
		          .modifier_at = TRUE},
		['^']  = {&teco_state_control,
		          .modifier_at = TRUE, .modifier_colon = 2},
		['F']  = {&teco_state_fcommand,
		          .modifier_at = TRUE, .modifier_colon = 2},
		['"']  = {&teco_state_condcommand},
		['E']  = {&teco_state_ecommand,
		          .modifier_at = TRUE, .modifier_colon = 2},
		['I']  = {&teco_state_insert_plain,
		          .modifier_at = TRUE},
		['?']  = {&teco_state_help,
		          .modifier_at = TRUE},
		['S']  = {&teco_state_search,
		          .modifier_at = TRUE, .modifier_colon = 2},
		['N']  = {&teco_state_search_all,
		          .modifier_at = TRUE, .modifier_colon = 1},

		['[']  = {&teco_state_pushqreg},
		[']']  = {&teco_state_popqreg,
		          .modifier_colon = 1},
		['G']  = {&teco_state_getqregstring,
		          .modifier_colon = 1},
		['Q']  = {&teco_state_queryqreg,
		          .modifier_colon = 1},
		['U']  = {&teco_state_setqreginteger,
		          .modifier_at = TRUE, .modifier_colon = 1},
		['%']  = {&teco_state_increaseqreg},
		['M']  = {&teco_state_macro,
		          .modifier_colon = 1},
		['X']  = {&teco_state_copytoqreg,
		          .modifier_at = TRUE, .modifier_colon = 1},
		['=']  = {&teco_state_print_decimal,
		          .modifier_colon = 1},

		/*
		 * Arithmetics
		 */
		['*']  = {&teco_state_start, teco_state_start_mul},
		['/']  = {&teco_state_start, teco_state_start_div},
		['+']  = {&teco_state_start, teco_state_start_plus},
		['-']  = {&teco_state_start, teco_state_start_minus},
		['&']  = {&teco_state_start, teco_state_start_and},
		['#']  = {&teco_state_start, teco_state_start_or},
		['(']  = {&teco_state_start, teco_state_start_brace_open},
		[')']  = {&teco_state_start, teco_state_start_brace_close},
		[',']  = {&teco_state_start, teco_state_start_comma},

		['.']  = {&teco_state_start, teco_state_start_dot},
		['Z']  = {&teco_state_start, teco_state_start_zed},
		['H']  = {&teco_state_start, teco_state_start_range},
		['\\'] = {&teco_state_start, teco_state_start_backslash},

		/*
		 * Control Structures (loops)
		 */
		['<']  = {&teco_state_start, teco_state_start_loop_open,
		          .modifier_colon = 1},
		['>']  = {&teco_state_start, teco_state_start_loop_close,
		          .modifier_colon = 1},
		[';']  = {&teco_state_start, teco_state_start_break,
		          .modifier_colon = 1},

		/*
		 * Command-line Editing
		 */
		['{']  = {&teco_state_start, teco_state_start_cmdline_push},
		['}']  = {&teco_state_start, teco_state_start_cmdline_pop},

		/*
		 * Commands
		 */
		['J']  = {&teco_state_start, teco_state_start_jump,
		          .modifier_colon = 1},
		['C']  = {&teco_state_start, teco_state_start_move,
		          .modifier_colon = 1},
		['R']  = {&teco_state_start, teco_state_start_reverse,
		          .modifier_colon = 1},
		['L']  = {&teco_state_start, teco_state_start_line,
		          .modifier_colon = 1},
		['B']  = {&teco_state_start, teco_state_start_back,
		          .modifier_colon = 1},
		['K']  = {&teco_state_start, teco_state_start_kill_lines,
		          .modifier_colon = 1},
		['D']  = {&teco_state_start, teco_state_start_delete_chars,
		          .modifier_colon = 1},
		['A']  = {&teco_state_start, teco_state_start_get},
		['T']  = {&teco_state_start, teco_state_start_typeout}
	};

	/*
	 * Non-operational commands.
	 * These are explicitly not handled in teco_state_control,
	 * so that we can potentially reuse the upcaret notations like ^J.
	 */
	if (teco_is_noop(chr)) {
		if (ctx->flags.modifier_at ||
		    (ctx->flags.mode == TECO_MODE_NORMAL && ctx->flags.modifier_colon)) {
			teco_error_modifier_set(error, chr);
			return NULL;
		}
		return &teco_state_start;
	}

	switch (chr) {
	/*$ 0 1 2 3 4 5 6 7 8 9 digit number
	 * [n]0|1|2|3|4|5|6|7|8|9 -> n*Radix+X -- Append digit
	 *
	 * Integer constants in \*(ST may be thought of and are
	 * technically sequences of single-digit commands.
	 * These commands take one argument from the stack
	 * (0 is implied), multiply it with the current radix
	 * (2, 8, 10, 16, ...), add the digit's value and
	 * return the resultant integer.
	 *
	 * The command-like semantics of digits may be abused
	 * in macros, for instance to append digits to computed
	 * integers.
	 * It is not an error to append a digit greater than the
	 * current radix - this may be changed in the future.
	 */
	case '0' ... '9':
		if (ctx->flags.modifier_at ||
		    (ctx->flags.mode == TECO_MODE_NORMAL && ctx->flags.modifier_colon)) {
			teco_error_modifier_set(error, chr);
			return NULL;
		}
		if (ctx->flags.mode == TECO_MODE_NORMAL)
			teco_expressions_add_digit(chr, ctx->qreg_table_locals->radix);
		return &teco_state_start;

	case '*':
		/*
		 * Special save last commandline command
		 *
		 * FIXME: Maybe, there should be a special teco_state_t
		 * for beginnings of command-lines?
		 * It could also be used for a corresponding KEYMACRO mask.
		 */
		if (teco_cmdline.effective_len == 1 && teco_cmdline.str.data[0] == '*')
			return &teco_state_save_cmdline;
		break;

	case '<':
		if (ctx->flags.modifier_at) {
			g_set_error_literal(error, TECO_ERROR, TECO_ERROR_MODIFIER,
			                    "Unexpected modifier on loop start");
			return NULL;
		}
		if (ctx->flags.mode != TECO_MODE_PARSE_ONLY_LOOP)
			break;
		if (ctx->parent.must_undo)
			teco_undo_gint(ctx->nest_level);
		ctx->nest_level++;
		return &teco_state_start;

	case '>':
		if (ctx->flags.modifier_at) {
			g_set_error_literal(error, TECO_ERROR, TECO_ERROR_MODIFIER,
			                    "Unexpected modifier on loop end");
			return NULL;
		}
		if (ctx->flags.mode != TECO_MODE_PARSE_ONLY_LOOP)
			break;
		if (!ctx->nest_level) {
			if (ctx->parent.must_undo)
				teco_undo_flags(ctx->flags);
			ctx->flags.mode = TECO_MODE_NORMAL;
		} else {
			if (ctx->parent.must_undo)
				teco_undo_gint(ctx->nest_level);
			ctx->nest_level--;
		}
		return &teco_state_start;

	/*
	 * Control Structures (conditionals)
	 */
	case '|':
		if (ctx->flags.modifier_at ||
		    (ctx->flags.mode == TECO_MODE_NORMAL && ctx->flags.modifier_colon)) {
			teco_error_modifier_set(error, '|');
			return NULL;
		}
		if (ctx->parent.must_undo)
			teco_undo_flags(ctx->flags);
		if (ctx->flags.mode == TECO_MODE_PARSE_ONLY_COND && !ctx->nest_level)
			ctx->flags.mode = TECO_MODE_NORMAL;
		else if (ctx->flags.mode == TECO_MODE_NORMAL)
			/* skip to end of conditional; skip ELSE-part */
			ctx->flags.mode = TECO_MODE_PARSE_ONLY_COND;
		return &teco_state_start;

	case '\'':
		if (ctx->flags.modifier_at ||
		    (ctx->flags.mode == TECO_MODE_NORMAL && ctx->flags.modifier_colon)) {
			teco_error_modifier_set(error, '\'');
			return NULL;
		}
		switch (ctx->flags.mode) {
		case TECO_MODE_PARSE_ONLY_COND:
		case TECO_MODE_PARSE_ONLY_COND_FORCE:
			if (!ctx->nest_level) {
				if (ctx->parent.must_undo)
					teco_undo_flags(ctx->flags);
				ctx->flags.mode = TECO_MODE_NORMAL;
			} else {
				if (ctx->parent.must_undo)
					teco_undo_gint(ctx->nest_level);
				ctx->nest_level--;
			}
			break;
		default:
			break;
		}
		return &teco_state_start;

	/*
	 * Word movement and deletion commands.
	 * These are not in the transitions table, so we can
	 * evaluate the @-modifier.
	 *
	 * All of these commands support both : and @-modifiers.
	 *
	 * FIXME: This will currently accept two colons as well,
	 * but should accept only one colon modifier.
	 */
	case 'w':
	case 'W': return teco_state_start_words(ctx, "W", 1, error);
	case 'p':
	case 'P': return teco_state_start_words(ctx, "P", -1, error);
	case 'v':
	case 'V': return teco_state_start_delete_words(ctx, "V", 1, error);
	case 'y':
	case 'Y': return teco_state_start_delete_words(ctx, "Y", -1, error);

	/*
	 * Modifiers
	 */
	case '@':
		if (ctx->flags.modifier_at) {
			teco_error_modifier_set(error, '@');
			return NULL;
		}
		/*
		 * @ modifier has syntactic significance, so set it even
		 * in PARSE_ONLY* modes.
		 * Unfortunately, it means that it must be evaluated and cleared
		 * everywhere, even where it has no syntactic significance.
		 */
		if (ctx->parent.must_undo)
			teco_undo_flags(ctx->flags);
		ctx->flags.modifier_at = TRUE;
		return &teco_state_start;

	case ':':
		if (ctx->flags.mode > TECO_MODE_NORMAL)
			return &teco_state_start;
		if (ctx->flags.modifier_colon >= 2) {
			teco_error_modifier_set(error, ':');
			return NULL;
		}
		if (ctx->parent.must_undo)
			teco_undo_flags(ctx->flags);
		ctx->flags.modifier_colon++;
		return &teco_state_start;

	default:
		/*
		 * <CTRL/x> commands implemented in teco_state_control
		 */
		if (TECO_IS_CTL(chr))
			return teco_state_control_input(ctx, TECO_CTL_ECHO(chr), error);
	}

	return teco_machine_main_transition_input(ctx, transitions, G_N_ELEMENTS(transitions),
	                                          teco_ascii_toupper(chr), error);
}

TECO_DEFINE_STATE_START(teco_state_start);

/*$ "F<" ":F<"
 * F< -- Go to loop start or jump to beginning of macro
 * :F<
 *
 * Immediately jumps to the current loop's start.
 * Also works from inside conditionals.
 *
 * This command behaves exactly like \fB<\fP with regard to
 * colon-modifiers.
 *
 * Outside of loops \(em or in a macro without
 * a loop \(em this jumps to the beginning of the macro.
 */
static void
teco_state_fcommand_loop_start(teco_machine_main_t *ctx, GError **error)
{
	if (teco_loop_stack->len <= ctx->loop_stack_fp) {
		/* outside of loop */
		if (!teco_expressions_discard_args(error))
			return;
		ctx->macro_pc = 0;
		return;
	}

	teco_loop_context_t *lctx = &g_array_index(teco_loop_stack, teco_loop_context_t,
	                                           teco_loop_stack->len-1);
	gboolean colon_modified = teco_machine_main_eval_colon(ctx) > 0;

	if (!lctx->pass_through) {
		if (colon_modified) {
			if (!teco_expressions_brace_close(error))
				return;
			teco_expressions_brace_open();
		} else if (!teco_expressions_discard_args(error)) {
			return;
		}
	}

	ctx->macro_pc = lctx->pc;
}

/*$ "F>" ":F>" continue
 * F> -- Go to loop end or return from macro
 * :F>
 *
 * Jumps to the current loop's end.
 * If the loop has remaining iterations or runs indefinitely,
 * the jump is performed immediately just as if \(lq>\(rq
 * had been executed.
 * If the loop has reached its last iteration, \*(ST will
 * parse until the loop end command has been found and control
 * resumes after the end of the loop.
 *
 * In interactive mode, if the loop is incomplete and must
 * be exited, you can type in the loop's remaining commands
 * without them being executed (but they are parsed).
 *
 * This command behaves exactly like \fB>\fP with regard to
 * colon-modifiers.
 *
 * Calling \fBF>\fP outside of a loop at the current
 * macro invocation level is equivalent to calling <$$>
 * (terminate command line or return from macro).
 */
static void
teco_state_fcommand_loop_end(teco_machine_main_t *ctx, GError **error)
{
	if (teco_loop_stack->len <= ctx->loop_stack_fp) {
		/* outside of loop */
		ctx->parent.current = &teco_state_start;
		if (!teco_expressions_eval(FALSE, error))
			return;
		teco_error_return_set(error, teco_expressions_args());
		return;
	}

	guint old_len = teco_loop_stack->len;

	/*
	 * NOTE: This is almost identical to the normal
	 * loop end since we don't really want to or need to
	 * parse till the end of the loop.
	 */
	g_assert(error != NULL);
	teco_state_start_loop_close(ctx, error);
	if (*error)
		return;

	if (teco_loop_stack->len < old_len) {
		/* skip to end of loop */
		if (ctx->parent.must_undo)
			teco_undo_flags(ctx->flags);
		ctx->flags.mode = TECO_MODE_PARSE_ONLY_LOOP;
	}
}

/*$ "F'"
 * F\' -- Jump to end of conditional
 */
static void
teco_state_fcommand_cond_end(teco_machine_main_t *ctx, GError **error)
{
	/* skip to end of conditional, also including any else-clause */
	if (ctx->parent.must_undo)
		teco_undo_flags(ctx->flags);
	ctx->flags.mode = TECO_MODE_PARSE_ONLY_COND_FORCE;
}

/*$ F|
 * F| -- Jump to else-part of conditional
 *
 * Jump to else-part of conditional or end of
 * conditional (only if invoked from inside the
 * condition's else-part).
 */
static void
teco_state_fcommand_cond_else(teco_machine_main_t *ctx, GError **error)
{
	/* skip to ELSE-part or end of conditional */
	if (ctx->parent.must_undo)
		teco_undo_flags(ctx->flags);
	ctx->flags.mode = TECO_MODE_PARSE_ONLY_COND;
}

static teco_state_t *
teco_state_fcommand_input(teco_machine_main_t *ctx, gunichar chr, GError **error)
{
	static teco_machine_main_transition_t transitions[] = {
		/*
		 * Simple transitions
		 */
		['K']  = {&teco_state_search_kill,
		          .modifier_at = TRUE, .modifier_colon = 1},
		['D']  = {&teco_state_search_delete,
		          .modifier_at = TRUE, .modifier_colon = 2},
		['S']  = {&teco_state_replace,
		          .modifier_at = TRUE, .modifier_colon = 2},
		['R']  = {&teco_state_replace_default,
		          .modifier_at = TRUE, .modifier_colon = 2},
		['N']  = {&teco_state_replace_default_all,
		          .modifier_at = TRUE, .modifier_colon = 1},
		['G']  = {&teco_state_changedir,
		          .modifier_at = TRUE},

		/*
		 * Loop Flow Control
		 */
		['<']  = {&teco_state_start, teco_state_fcommand_loop_start,
		          .modifier_colon = 1},
		['>']  = {&teco_state_start, teco_state_fcommand_loop_end,
		          .modifier_colon = 1},

		/*
		 * Conditional Flow Control
		 */
		['\''] = {&teco_state_start, teco_state_fcommand_cond_end},
		['|']  = {&teco_state_start, teco_state_fcommand_cond_else}
	};

	return teco_machine_main_transition_input(ctx, transitions, G_N_ELEMENTS(transitions),
	                                          teco_ascii_toupper(chr), error);
}

TECO_DEFINE_STATE_COMMAND(teco_state_fcommand);

static void
teco_undo_change_dir_action(gchar **dir, gboolean run)
{
	/*
	 * Changing the directory on rub-out may fail.
	 * This is handled silently.
	 */
	if (run)
		g_chdir(*dir);
	g_free(*dir);
}

void
teco_undo_change_dir_to_current(void)
{
	gchar **ctx = teco_undo_push_size((teco_undo_action_t)teco_undo_change_dir_action,
	                                  sizeof(gchar *));
	if (ctx)
		*ctx = g_get_current_dir();
}

static teco_state_t *
teco_state_changedir_done(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	if (ctx->flags.mode > TECO_MODE_NORMAL)
		return &teco_state_start;

	g_autofree gchar *dir = teco_file_expand_path(str->data);
	if (!*dir) {
		teco_qreg_t *qreg = teco_qreg_table_find(&teco_qreg_table_globals, "$HOME", 5);
		g_assert(qreg != NULL);
		teco_string_t home;
		if (!qreg->vtable->get_string(qreg, &home.data, &home.len, NULL, error))
			return NULL;

		/*
		 * Null-characters must not occur in file names.
		 */
		if (teco_string_contains(&home, '\0')) {
			teco_string_clear(&home);
			g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
			                    "Null-character not allowed in filenames");
			return NULL;
		}
		g_assert(home.data != NULL);

		g_free(dir);
		dir = home.data;
	}

	teco_undo_change_dir_to_current();

	if (g_chdir(dir)) {
		/* FIXME: Is errno usable on Windows here? */
		g_set_error(error, TECO_ERROR, TECO_ERROR_FAILED,
		            "Cannot change working directory to \"%s\"", dir);
		return NULL;
	}

	return &teco_state_start;
}

/*$ FG cd change-dir folder-go
 * FG[directory]$ -- Change working directory
 *
 * Changes the process' current working directory
 * to <directory> which affects all subsequent
 * operations on relative file names like
 * tab-completions.
 * It is also inherited by external processes spawned
 * via \fBEC\fP and \fBEG\fP.
 *
 * If <directory> is omitted, the working directory
 * is changed to the current user's home directory
 * as set by the \fBHOME\fP environment variable
 * (i.e. its corresponding \(lq$HOME\(rq environment
 * register).
 * This variable is always initialized by \*(ST
 * (see \fBsciteco\fP(1)).
 * Therefore the expression \(lqFG\fB$\fP\(rq is
 * exactly equivalent to both \(lqFG~\fB$\fP\(rq and
 * \(lqFG^EQ[$HOME]\fB$\fP\(rq.
 *
 * The current working directory is also mapped to
 * the special global Q-Register \(lq$\(rq (dollar sign)
 * which may be used retrieve the current working directory.
 *
 * String-building characters are enabled on this
 * command and directories can be tab-completed.
 */
TECO_DEFINE_STATE_EXPECTDIR(teco_state_changedir);

static teco_state_t *
teco_state_condcommand_input(teco_machine_main_t *ctx, gunichar chr, GError **error)
{
	teco_int_t value = 0;
	gboolean result = TRUE;

	switch (ctx->flags.mode) {
	case TECO_MODE_PARSE_ONLY_COND:
	case TECO_MODE_PARSE_ONLY_COND_FORCE:
		if (ctx->parent.must_undo)
			teco_undo_gint(ctx->nest_level);
		ctx->nest_level++;
		break;

	case TECO_MODE_NORMAL:
		if (!teco_expressions_eval(FALSE, error))
			return NULL;

		if (chr == '~')
			/* don't pop value for ~ conditionals */
			break;

		if (!teco_expressions_args()) {
			teco_error_argexpected_set(error, "\"");
			return NULL;
		}
		value = teco_expressions_pop_num(0);
		break;

	default:
		break;
	}

	switch (teco_ascii_toupper(chr)) {
	case '~':
		if (ctx->flags.mode == TECO_MODE_NORMAL)
			result = !teco_expressions_args();
		break;
	case 'A':
		if (ctx->flags.mode == TECO_MODE_NORMAL)
			result = g_unichar_isalpha(value);
		break;
	case 'C':
		if (ctx->flags.mode == TECO_MODE_NORMAL)
			result = g_unichar_isalnum(value) ||
			         value == '.' || value == '$' || value == '_';
		break;
	case 'D':
		if (ctx->flags.mode == TECO_MODE_NORMAL)
			result = g_unichar_isdigit(value);
		break;
	case 'I':
		if (ctx->flags.mode == TECO_MODE_NORMAL)
			result = G_IS_DIR_SEPARATOR(value);
		break;
	case 'S':
	case 'T':
		if (ctx->flags.mode == TECO_MODE_NORMAL)
			result = teco_is_success(value);
		break;
	case 'F':
	case 'U':
		if (ctx->flags.mode == TECO_MODE_NORMAL)
			result = teco_is_failure(value);
		break;
	case 'E':
	case '=':
		if (ctx->flags.mode == TECO_MODE_NORMAL)
			result = value == 0;
		break;
	case 'G':
	case '>':
		if (ctx->flags.mode == TECO_MODE_NORMAL)
			result = value > 0;
		break;
	case 'L':
	case '<':
		if (ctx->flags.mode == TECO_MODE_NORMAL)
			result = value < 0;
		break;
	case 'N':
		if (ctx->flags.mode == TECO_MODE_NORMAL)
			result = value != 0;
		break;
	case 'R':
		if (ctx->flags.mode == TECO_MODE_NORMAL)
			result = g_unichar_isalnum(value);
		break;
	case 'V':
		if (ctx->flags.mode == TECO_MODE_NORMAL)
			result = g_unichar_islower(value);
		break;
	case 'W':
		if (ctx->flags.mode == TECO_MODE_NORMAL)
			result = g_unichar_isupper(value);
		break;
	default:
		g_set_error(error, TECO_ERROR, TECO_ERROR_FAILED,
		            "Invalid conditional type \"%c\"", chr);
		return NULL;
	}

	if (!result) {
		/* skip to ELSE-part or end of conditional */
		if (ctx->parent.must_undo)
			teco_undo_flags(ctx->flags);
		ctx->flags.mode = TECO_MODE_PARSE_ONLY_COND;
	}

	return &teco_state_start;
}

TECO_DEFINE_STATE_COMMAND(teco_state_condcommand,
	.style = SCE_SCITECO_OPERATOR
);

/*$ ^_ negate
 * n^_ -> ~n -- Binary negation
 *
 * Binary negates (complements) <n> and returns
 * the result.
 * Binary complements are often used to negate
 * \*(ST booleans.
 */
static void
teco_state_control_negate(teco_machine_main_t *ctx, GError **error)
{
	if (!teco_expressions_eval(FALSE, error))
		return;

	if (!teco_expressions_args()) {
		teco_error_argexpected_set(error, "^_");
		return;
	}

	teco_expressions_push(~teco_expressions_pop_num(0));
}

static void
teco_state_control_pow(teco_machine_main_t *ctx, GError **error)
{
	teco_expressions_push_calc(TECO_OP_POW, error);
}

static void
teco_state_control_mod(teco_machine_main_t *ctx, GError **error)
{
	teco_expressions_push_calc(TECO_OP_MOD, error);
}

static void
teco_state_control_xor(teco_machine_main_t *ctx, GError **error)
{
	teco_expressions_push_calc(TECO_OP_XOR, error);
}

/*$ ^O octal
 * ^O -- Set radix to 8 (octal)
 */
static void
teco_state_control_octal(teco_machine_main_t *ctx, GError **error)
{
	teco_qreg_t *qreg = ctx->qreg_table_locals->radix;
	assert(qreg != NULL);
	if (!qreg->vtable->undo_set_integer(qreg, error) ||
	    !qreg->vtable->set_integer(qreg, 8, NULL))
		return;
}

/*$ ^D decimal
 * ^D -- Set radix to 10 (decimal)
 */
static void
teco_state_control_decimal(teco_machine_main_t *ctx, GError **error)
{
	teco_qreg_t *qreg = ctx->qreg_table_locals->radix;
	assert(qreg != NULL);
	if (!qreg->vtable->undo_set_integer(qreg, error) ||
	    !qreg->vtable->set_integer(qreg, 10, NULL))
		return;
}

/*$ ^R radix
 * radix^R -- Set and get radix
 * ^R -> radix
 *
 * Set current radix to any value <radix> larger than or equal to 2.
 * If <radix> is omitted, the command instead
 * returns the current radix.
 *
 * An alternative way to access the radix is via the \(lq^R\(rq local Q-Register.
 * Consequently, the radix is local to the current macro invocation frame,
 * unless the macro call was colon-modified.
 */
static void
teco_state_control_radix(teco_machine_main_t *ctx, GError **error)
{
	if (!teco_expressions_eval(FALSE, error))
		return;

	teco_qreg_t *qreg = ctx->qreg_table_locals->radix;
	assert(qreg != NULL);
	teco_int_t radix;

	if (!teco_expressions_args()) {
		if (!qreg->vtable->get_integer(qreg, &radix, error))
			return;
		teco_expressions_push(radix);
	} else {
		if (!qreg->vtable->undo_set_integer(qreg, error) ||
		    !qreg->vtable->set_integer(qreg, teco_expressions_pop_num(0), error))
			return;
	}
}

/*$ "^E" ":^E" glyphs2bytes bytes2glyphs
 * glyphs^E -> bytes -- Translate between glyph and byte indexes
 * bytes:^E -> glyphs
 * ^E -> bytes
 * :^E -> length
 *
 * Translates from glyph/character to byte indexes when called
 * without a colon.
 * Otherwise when colon-modified, translates from byte indexes
 * back to glyph indexes.
 * These values can differ in documents with multi-byte
 * encodings (of which only UTF-8 is supported).
 * It is especially useful to translate between these indexes
 * when manually invoking Scintilla messages (\fBES\fP command), as
 * they almost always take byte positions.
 *
 * When called without arguments, \fB^E\fP returns the current
 * position (dot) in bytes.
 * This is equivalent, but faster than \(lq.^E\(rq.
 * \fB:^E\fP without arguments returns the length of the current
 * document in bytes, which is equivalent but faster than \(lqZ^E\(rq.
 *
 * When passing in indexes outside of the document's valid area,
 * -1 is returned, so the return value can also be interpreted
 * as a TECO boolean, signalling truth/success for invalid indexes.
 * This provides an elegant and effective way to validate
 * buffer addresses.
 */
static void
teco_state_control_glyphs2bytes(teco_machine_main_t *ctx, GError **error)
{
	teco_int_t res;

	if (!teco_expressions_eval(FALSE, error))
		return;

	gboolean colon_modified = teco_machine_main_eval_colon(ctx) > 0;

	if (!teco_expressions_args()) {
		/*
		 * This is shorter than .^E or Z^E and avoids unnecessary glyph to
		 * byte index translations.
		 * On the other hand :^E is inconsistent, as it will return a byte
		 * index, instead of glyph index.
		 */
		res = teco_interface_ssm(colon_modified ? SCI_GETLENGTH : SCI_GETCURRENTPOS, 0, 0);
	} else {
		teco_int_t pos = teco_expressions_pop_num(0);
		if (colon_modified) {
			/* teco_interface_bytes2glyphs() does not check addresses */
			res = 0 <= pos && pos <= teco_interface_ssm(SCI_GETLENGTH, 0, 0)
				? teco_interface_bytes2glyphs(pos) : -1;
		} else {
			/* negative values for invalid indexes are passed down. */
			res = teco_interface_glyphs2bytes(pos);
		}
	}

	teco_expressions_push(res);
}

/**
 * Number of buffer ranges in teco_ranges
 * @fixme Should this be 1 from the very beginning, so 0^Y/^S never fail?
 */
guint teco_ranges_count = 0;
/** Array of buffer ranges of the last matched substrings or the last text insertion */
teco_range_t *teco_ranges = NULL;

/*
 * Make sure we always have space for at least one result,
 * so we don't have to check for NULL everywhere.
 */
static void __attribute__((constructor))
teco_ranges_init(void)
{
	teco_ranges = g_new0(teco_range_t, 1);
}

/*$ ^Y subexpression subpattern
 * [n]^Y -> start, end -- Return range of last pattern match, subexpression or text insertion
 *
 * This command returns the buffer ranges of the subpatterns of the
 * last pattern match (search command) or of the last text insertion.
 * <n> specifies the number of the subpattern from left to right.
 * The default value 0 specifies the entire matched pattern,
 * while higher numbers refer to \fB^E[\fI...\fB]\fR subpatterns.
 * \fB^Y\fP can also be used to return the buffer range of the
 * last text insertion by any \*(ST command (\fBI\fP, \fB^I\fP, \fBG\fIq\fR,
 * \fB\\\fP, \fBEC\fP, \fBEN\fP, search replacements, etc).
 * In this case <n> is only allowed to be 0 or missing.
 *
 * For instance, \(lq^YXq\(rq copies the entire matched pattern or text
 * insertion into register \fIq\fP.
 */
/*
 * In DEC TECO, this is actually defined as ".+^S,.".
 * The SciTECO version is more robust to moving dot afterwards, though,
 * as it will always return the same buffer range.
 */
static void
teco_state_control_last_range(teco_machine_main_t *ctx, GError **error)
{
	teco_int_t n;

	if (!teco_expressions_pop_num_calc(&n, 0, error))
		return;
	if (n < 0 || n >= teco_ranges_count) {
		teco_error_subpattern_set(error, "^Y");
		return;
	}

	teco_expressions_push(teco_ranges[n].from);
	teco_expressions_push(teco_ranges[n].to);
}

/*$ ^S
 * [n]^S -> -length -- Return negative length of last pattern match, subexpression or text insertion
 * -^S -> length
 *
 * Returns the negative length of the subpatterns of the last pattern match
 * (search command) or of the last text insertion.
 * <n> specifies the number of the subpattern from left to right
 * and defaults to 0 (the entire pattern match or text insertion).
 * \(lq^S\(rq is equivalent to \(lq^YU1U0 Q0-Q1\(rq.
 * Without arguments, the sign prefix negates the result, i.e. returns the
 * length of the entire matched pattern or text insertion.
 *
 * A common idiom \(lq^SC\(rq can be used for jumping to the
 * beginning of the matched pattern or inserted string.
 * Since the result is always negative, you can use \(lq^SR\(rq
 * to skip the matched pattern after \fBFK\fP.
 */
static void
teco_state_control_last_length(teco_machine_main_t *ctx, GError **error)
{
	teco_int_t n = 0;

	/*
	 * There is little use in supporting n^S for n != 0.
	 * This is just for consistency with ^Y.
	 *
	 * We do not use teco_expressions_pop_num_calc(),
	 * so as not to reset the sign prefix.
	 */
	if (!teco_expressions_eval(FALSE, error))
		return;
	if (teco_expressions_args() > 0)
		n = teco_expressions_pop_num(0);
	if (n < 0 || n >= teco_ranges_count) {
		teco_error_subpattern_set(error, "^S");
		return;
	}

	teco_expressions_push(teco_ranges[n].from - teco_ranges[n].to);
}

static void TECO_DEBUG_CLEANUP
teco_ranges_cleanup(void)
{
	g_free(teco_ranges);
}

/*$ ^B date
 * ^B -> (((year-1900)*16 + month)*32 + day) -- Retrieve date
 *
 * Returns the current date via the given equation.
 */
/*
 * FIXME: Perhaps :^B should directly return the
 * decoded year, month and day.
 */
static void
teco_state_control_date(teco_machine_main_t *ctx, GError **error)
{
	GDate date;

	g_date_clear(&date, 1);
	g_date_set_time_t(&date, time(NULL));
	teco_expressions_push(((g_date_get_year(&date)-1900)*16 + g_date_get_month(&date))*32 +
	                      g_date_get_day(&date));
}

/*$ "^H" ":^H" "::^H" time timestamp
 * ^H -> seconds since midnight -- Retrieve time of day or timestamp
 * :^H -> seconds
 * ::^H -> timestamp
 *
 * By default returns the current time in seconds since midnight (UTC).
 *
 * If colon-modified it returns the number of <seconds> since the Epoch,
 * 1970-01-01 00:00:00 +0000 (UTC).
 *
 * If modified by two colons it returns the system's monotonic time in microseconds,
 * which can be used as a <timestamp>.
 */
static void
teco_state_control_time(teco_machine_main_t *ctx, GError **error)
{
	switch (teco_machine_main_eval_colon(ctx)) {
	case 0:
		teco_expressions_push(time(NULL) % (60*60*24));
		break;
	case 1:
		teco_expressions_push(time(NULL));
		break;
	case 2:
		/*
		 * NOTE: Might not be reliable if TECO_INTEGER==32.
		 */
		teco_expressions_push(g_get_monotonic_time());
		break;
	default:
		g_assert_not_reached();
	}
}

/*$ ^W refresh sleep delay wait
 * [n]^W -- Wait and refresh screen
 *
 * First sleep <n> milliseconds before refreshing the view,
 * i.e. drawing it.
 * By default it sleeps for 10ms.
 * This can be added to loops to make progress visible
 * in interactive mode.
 * In batch mode this command is useful as a sleep command.
 * Sleeps can of course be interrupted with CTRL+C.
 *
 * Since CTRL+W is an immediate editing command, you may
 * have to type this command in upcaret mode.
 * To enforce a complete screen redraw you can also
 * press CTRL+L.
 */
static void
teco_state_control_refresh(teco_machine_main_t *ctx, GError **error)
{
	teco_int_t ms;

	if (!teco_expressions_pop_num_calc(&ms, 10, error))
		return;

	while (ms > 0 && !teco_interface_is_interrupted()) {
		/*
		 * UNIX' usleep() would also be interrupted by
		 * SIGINT, but polling for interruptions is
		 * probably precise enough.
		 * We need this as a fallback anyway.
		 */
		g_usleep(MIN(ms*1000, TECO_POLL_INTERVAL));
		ms -= TECO_POLL_INTERVAL/1000;
	}

	teco_interface_unfold();
	teco_interface_ssm(SCI_SCROLLCARET, 0, 0);
	teco_interface_refresh(FALSE);
}

static teco_state_t *
teco_state_control_input(teco_machine_main_t *ctx, gunichar chr, GError **error)
{
	static teco_machine_main_transition_t transitions[] = {
		/*
		 * Simple transitions
		 */
		['I']  = {&teco_state_insert_indent,
		          .modifier_at = TRUE},
		['U']  = {&teco_state_ctlucommand,
		          .modifier_at = TRUE, .modifier_colon = 1},
		['^']  = {&teco_state_ascii},
		['[']  = {&teco_state_escape},
		['C']  = {&teco_state_ctlc},
		['A']  = {&teco_state_print_string,
		          .modifier_at = TRUE, .modifier_colon = 1},

		/*
		 * Additional numeric operations
		 */
		['_']  = {&teco_state_start, teco_state_control_negate},
		['*']  = {&teco_state_start, teco_state_control_pow},
		['/']  = {&teco_state_start, teco_state_control_mod},
		['#']  = {&teco_state_start, teco_state_control_xor},

		/*
		 * Commands
		 */
		['B']  = {&teco_state_start, teco_state_control_date},
		['H']  = {&teco_state_start, teco_state_control_time,
		          .modifier_colon = 2},
		['O']  = {&teco_state_start, teco_state_control_octal},
		['D']  = {&teco_state_start, teco_state_control_decimal},
		['R']  = {&teco_state_start, teco_state_control_radix},
		['Q']  = {&teco_state_start, teco_state_control_lines2glyphs,
		          .modifier_colon = 1},
		['E']  = {&teco_state_start, teco_state_control_glyphs2bytes,
		          .modifier_colon = 1},
		['X']  = {&teco_state_start, teco_state_control_search_mode},
		['Y']  = {&teco_state_start, teco_state_control_last_range},
		['S']  = {&teco_state_start, teco_state_control_last_length},
		['T']  = {&teco_state_start, teco_state_control_typeout,
		          .modifier_colon = 1},
		['W']  = {&teco_state_start, teco_state_control_refresh}
	};

	/*
	 * FIXME: Should we return a special syntax error in case of failure?
	 * Currently you get error messages like 'Syntax error "F"' for ^F.
	 * The easiest way around would be g_prefix_error(error, "Control command");
	 */
	return teco_machine_main_transition_input(ctx, transitions, G_N_ELEMENTS(transitions),
	                                          teco_ascii_toupper(chr), error);
}

TECO_DEFINE_STATE_COMMAND(teco_state_control);

static teco_state_t *
teco_state_ascii_input(teco_machine_main_t *ctx, gunichar chr, GError **error)
{
	if (ctx->flags.mode == TECO_MODE_NORMAL)
		teco_expressions_push(chr);

	return &teco_state_start;
}

/*$ ^^ ^^c
 * ^^c -> n -- Get ASCII code of character
 *
 * Returns the ASCII code of the character <c>
 * that is part of the command.
 * Can be used in place of integer constants for improved
 * readability.
 * For instance ^^A will return 65.
 *
 * Note that this command can be typed CTRL+Caret or
 * Caret-Caret.
 */
TECO_DEFINE_STATE(teco_state_ascii);

/*$ ^[^[ ^[$ $$ ^C terminate return
 * [a1,a2,...]$$ -- Terminate command line or return from macro
 * [a1,a2,...]^[$
 * [a1,a2,...]^C
 *
 * Returns from the current macro invocation.
 * This will pass control to the calling macro immediately
 * and is thus faster than letting control reach the macro's end.
 * Also, direct arguments to \fB$$\fP will be left on the expression
 * stack when the macro returns.
 * \fB$$\fP closes loops automatically and is thus safe to call
 * from loop bodies.
 * Furthermore, it has defined semantics when executed
 * from within braced expressions:
 * All braces opened in the current macro invocation will
 * be closed and their values discarded.
 * Only the direct arguments to \fB$$\fP will be kept.
 *
 * Returning from the top-level macro in batch mode
 * will exit the program or start up interactive mode depending
 * on whether program exit has been requested.
 * If \fB$$\fP exits the program, any remaining numeric parameter
 * is returned by the process as its exit status.
 * By default, the success code is returned.
 * \(lqEX\fB$$\fP\(rq is thus a common idiom to exit
 * prematurely.
 *
 * In interactive mode, returning from the top-level macro
 * (i.e. typing \fB$$\fP at the command line) has the
 * effect of command line termination.
 * The arguments to \fB$$\fP are currently not used
 * when terminating a command line \(em the new command line
 * will always start with a clean expression stack.
 *
 * \fB^C\fP cannot be typed directly on the command-line
 * as it could be inserted accidentally after interrupting
 * operations with CTRL+C.
 *
 * The first \fIescape\fP of \fB$$\fP may be typed either
 * as an escape character (ASCII 27), in up-arrow mode
 * (e.g. \fB^[$\fP) or as a dollar character \(em the
 * second character must be either a real escape character
 * or a dollar character.
 */
/*
 * FIXME: Analogous to ^C^C, we could support ^[^[ typed with carets only
 * at the expense of yet another parser state.
 */
static teco_state_t *
teco_return(teco_machine_main_t *ctx, GError **error)
{
	g_assert(ctx->flags.mode == TECO_MODE_NORMAL);

	/*
	 * This check is not crucial, but a return command would
	 * terminate the command line and it would be impossible to apply the new
	 * command line with `}` after command-line termination.
	 */
	if (G_UNLIKELY(ctx == &teco_cmdline.machine &&
	               teco_qreg_current && !teco_string_cmp(&teco_qreg_current->head.name, "\e", 1))) {
		g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
		                    "Not allowed to terminate command-line while "
		                    "editing command-line replacement register");
		return NULL;
	}

	ctx->parent.current = &teco_state_start;
	if (!teco_expressions_eval(FALSE, error))
		return NULL;
	teco_error_return_set(error, teco_expressions_args());
	return NULL;
}

/*
 * The Escape state is special, as it implements
 * a kind of "lookahead" for the ^[ command (discard all
 * arguments).
 * It is not executed immediately as usual in SciTECO
 * but only if not followed by an escape character.
 * This is necessary since $$ is the macro return
 * and command-line termination command and it must not
 * discard arguments.
 * Deferred execution of ^[ is possible since it does
 * not have any visible side-effects - its effects can
 * only be seen when executing the following command.
 */
static teco_state_t *
teco_state_escape_input(teco_machine_main_t *ctx, gunichar chr, GError **error)
{
	if (chr == '\e' || chr == '$')
		return ctx->flags.mode > TECO_MODE_NORMAL
			? &teco_state_start : teco_return(ctx, error);

	/*
	 * Alternatives: ^[, <CTRL/[>, <ESC>, $ (dollar)
	 */
	/*$ ^[ $ escape discard
	 * $ -- Discard all arguments
	 * ^[
	 *
	 * Pops and discards all values from the stack that
	 * might otherwise be used as arguments to following
	 * commands.
	 * Therefore it stops popping on stack boundaries like
	 * they are introduced by arithmetic brackets or loops.
	 *
	 * Note that ^[ is usually typed using the Escape key.
	 * CTRL+[ however is possible as well and equivalent to
	 * Escape in every manner.
	 * The up-arrow notation however is processed like any
	 * ordinary command and only works at the begining of
	 * a command.
	 * Additionally, this command may be written as a single
	 * dollar character.
	 */
	if (ctx->flags.mode == TECO_MODE_NORMAL &&
	    !teco_expressions_discard_args(error))
		return NULL;
	return teco_state_start_input(ctx, chr, error);
}

static gboolean
teco_state_escape_end_of_macro(teco_machine_t *ctx, GError **error)
{
	/*
	 * Due to the deferred nature of ^[,
	 * it is valid to end in the "escape" state.
	 */
	return teco_expressions_discard_args(error);
}

TECO_DEFINE_STATE_START(teco_state_escape,
	.end_of_macro_cb = teco_state_escape_end_of_macro
);

/*
 * Just like ^[, ^C actually implements a lookahead,
 * so a ^C itself does nothing.
 * This does not break the user experience since ^C
 * is disallowed to type at the command-line.
 */
static teco_state_t *
teco_state_ctlc_input(teco_machine_main_t *ctx, gunichar chr, GError **error)
{
	switch (chr) {
	case TECO_CTL_KEY('C'):	return teco_state_ctlc_control_input(ctx, 'C', error);
	case '^':		return &teco_state_ctlc_control;
	}

	return ctx->flags.mode > TECO_MODE_NORMAL
		? teco_state_start_input(ctx, chr, error) : teco_return(ctx, error);
}

static gboolean
teco_state_ctlc_initial(teco_machine_main_t *ctx, GError **error)
{
	if (G_UNLIKELY(ctx == &teco_cmdline.machine)) {
		g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
		                    "<^C> is not allowed to terminate command-lines");
		return FALSE;
	}

	return TRUE;
}

TECO_DEFINE_STATE_START(teco_state_ctlc,
	.initial_cb = (teco_state_initial_cb_t)teco_state_ctlc_initial
);

static teco_state_t *
teco_state_ctlc_control_input(teco_machine_main_t *ctx, gunichar chr, GError **error)
{
	/*$ ^C^C exit
	 * [n]^C^C -- Exit program immediately
	 *
	 * Lets the top-level macro return immediately
	 * regardless of the current macro invocation frame.
	 * This command is only allowed in batch mode,
	 * so it is not invoked accidentally when using
	 * the CTRL+C immediate editing command to
	 * interrupt long running operations.
	 * When using \fB^C^C\fP in a munged file,
	 * interactive mode is never started, so it behaves
	 * effectively just like \(lq-EX\fB$$\fP\(rq
	 * (when executed in the top-level macro at least).
	 *
	 * Any numeric parameter is returned by the process
	 * as its exit status.
	 * By default, the success code is returned.
	 * The \fBquit\fP hook is still executed.
	 *
	 * This command is currently disallowed in interactive mode.
	 *
	 * Note that both \(lq^C\(rq can be typed either
	 * as control codes (3) or with carets.
	 */
	if (chr == 'c' || chr == 'C') {
		if (ctx->flags.mode > TECO_MODE_NORMAL)
			return &teco_state_start;

		if (teco_undo_enabled) {
			g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
			                    "<^C^C> not allowed in interactive mode");
			return NULL;
		}

		if (!teco_expressions_eval(FALSE, error))
			return NULL;
		teco_ed |= TECO_ED_EXIT;
		g_set_error_literal(error, TECO_ERROR, TECO_ERROR_QUIT, "");
		return NULL;
	}

	return ctx->flags.mode > TECO_MODE_NORMAL
		? teco_state_control_input(ctx, chr, error) : teco_return(ctx, error);
}

/*
 * This state is necessary, so that you can type ^C^C exclusively with carets.
 * Otherwise it would be very cumbersome to cause exits with ASCII characters only.
 */
TECO_DEFINE_STATE_COMMAND(teco_state_ctlc_control);

/*$ ED flags
 * flags ED -- Set and get ED-flags
 * [off,]on ED
 * ED -> flags
 *
 * With arguments, the command will set the \fBED\fP flags.
 * <flags> is a bitmap of flags to set.
 * Specifying one argument to set the flags is a special
 * case of specifying two arguments that allow to control
 * which flags to enable/disable.
 * <off> is a bitmap of flags to disable (set to 0 in ED
 * flags) and <on> is a bitmap of flags that is ORed into
 * the flags variable.
 * If <off> is omitted, the value 0^_ is implied.
 * In otherwords, all flags are turned off before turning
 * on the <on> flags.
 * Without any argument ED returns the current flags.
 *
 * Currently, the following flags are used by \*(ST:
 * .IP 2: 5
 * Reflects whether program termination has been requested
 * by successfully performing the \fBEX\fP command.
 * This flag can also be used to cancel the effect of any
 * prior \fBEX\fP.
 * .IP 4:
 * If enabled, prefer raw single-byte ANSI encoding
 * for all new buffers and registers.
 * This does not change the encoding of any existing
 * buffers and any initialized default register when set via
 * \fBED\fP, so you might want to launch \*(ST with \fB--8bit\fP.
 * .IP 8:
 * Enable/disable automatic folding of case-insensitive
 * command characters during interactive key translation.
 * The case of letter keys is inverted, so one or two
 * character commands will typically be inserted upper-case,
 * but you can still press Shift to insert lower-case letters.
 * Case-insensitive Q-Register specifications are not
 * case folded.
 * This is thought to improve the readability of the command
 * line macro.
 * .IP 16:
 * Enable/disable automatic translation of end of
 * line sequences to and from line feed.
 * Disabling this flag allows 8-bit clean loading and saving
 * of files.
 * .IP 32:
 * Enable/Disable buffer editing hooks
 * (via execution of macro in global Q-Register \(lqED\(rq)
 * .IP 64:
 * .SCITECO_TOPIC mouse
 * Enable/Disable processing and delivery of mouse events in
 * the Curses UI.
 * If enabled, the terminal emulator's default mouse behavior
 * may be inhibited.
 * .IP 128:
 * Enable/Disable enforcement of UNIX98
 * \(lq/bin/sh\(rq emulation for operating system command
 * executions
 * .IP 256:
 * Enable/Disable OSC-52 clipboard support.
 * Must only be enabled if the terminal emulator is configured
 * properly.
 * .IP 512:
 * Enable/Disable Unicode icons and symbols in the Curses UI.
 * This requires a capable font, like the ones provided
 * by the \(lqNerd Fonts\(rq project.
 * Changes to this flag in interactive mode may not become
 * effective immediately.
 *
 * The features controlled thus are discribed in other sections
 * of this manual.
 *
 * The default value of the \fBED\fP flags is 16
 * (only automatic EOL translation enabled).
 */
static void
teco_state_ecommand_flags(teco_machine_main_t *ctx, GError **error)
{
	if (!teco_expressions_eval(FALSE, error))
		return;
	if (!teco_expressions_args()) {
		teco_expressions_push(teco_ed);
	} else {
		teco_int_t on, off;
		if (!teco_expressions_pop_num_calc(&on, 0, error) ||
		    !teco_expressions_pop_num_calc(&off, ~(teco_int_t)0, error))
			return;
		teco_undo_int(teco_ed) = (teco_ed & ~off) | on;
	}
}

/*$ EJ properties
 * [key]EJ -> value -- Get and set system properties
 * value,keyEJ
 * rgb,color,3EJ
 * -EJ -> event
 * -2EJ -> y, x
 *
 * This command may be used to get and set system
 * properties.
 * With one argument, it retrieves a numeric property
 * identified by \fIkey\fP.
 * If \fIkey\fP is omitted, the prefix sign is implied
 * (1 or -1).
 * With two arguments, it sets property \fIkey\fP to
 * \fIvalue\fP and returns nothing. Some property \fIkeys\fP
 * may require more than one value. Properties may be
 * write-only or read-only.
 *
 * The following property keys are defined:
 * .IP 0: 4
 * The current user interface: 1 for Curses, 2 for GTK
 * (\fBread-only\fP)
 * .IP 1:
 * The current number of buffers: Also the numeric id
 * of the last buffer in the ring. This is implied if
 * no argument is given, so \(lqEJ\(rq returns the number
 * of buffers in the ring.
 * (\fBread-only\fP)
 * .IP 2:
 * The current memory limit in bytes.
 * This limit helps to prevent dangerous out-of-memory
 * conditions (e.g. resulting from infinite loops) by
 * constantly sampling the memory requirements of \*(ST.
 * Note that not all platforms support precise measurements
 * of the current memory usage \(em \*(ST will fall back
 * to an approximation which might be less than the actual
 * usage on those platforms.
 * Memory limiting is effective in batch and interactive mode.
 * Commands which would exceed that limit will fail instead
 * allowing users to recover in interactive mode, e.g. by
 * terminating the command line.
 * When getting, a zero value indicates that memory limiting is
 * disabled.
 * Setting a value less than or equal to 0 as in
 * \(lq0,2EJ\(rq disables the limit.
 * \fBWarning:\fP Disabling memory limiting may provoke
 * out-of-memory errors in long running or infinite loops
 * (interactive mode) that result in abnormal program
 * termination.
 * Setting a new limit may fail if the current memory
 * requirements are too large for the new limit \(em if
 * this happens you may have to clear your command-line
 * first.
 * Memory limiting is enabled by default.
 * .IP 3:
 * This \fBwrite-only\fP property allows redefining the
 * first 16 entries of the terminal color palette \(em a
 * feature required by some
 * color schemes when using the Curses user interface.
 * When setting this property, you are making a request
 * to define the terminal \fIcolor\fP as the Scintilla-compatible
 * RGB color value given in the \fIrgb\fP parameter.
 * \fIcolor\fP must be a value between 0 and 15
 * corresponding to black, red, green, yellow, blue, magenta,
 * cyan, white, bright black, bright red, etc. in that order.
 * The \fIrgb\fP value has the format 0xBBGGRR, i.e. the red
 * component is the least-significant byte and all other bytes
 * are ignored.
 * Note that on curses, RGB color values sent to Scintilla
 * are actually mapped to these 16 colors by the Scinterm port
 * and may represent colors with no resemblance to the \(lqRGB\(rq
 * value used (depending on the current palette) \(em they should
 * instead be viewed as placeholders for 16 standard terminal
 * color codes.
 * Please refer to the Scinterm manual for details on the allowed
 * \(lqRGB\(rq values and how they map to terminal colors.
 * This command provides a crude way to request exact RGB colors
 * for the first 16 terminal colors.
 * The color definition may be queued or be completely ignored
 * on other user interfaces and no feedback is given
 * if it fails. In fact feedback cannot be given reliably anyway.
 * Note that on 8 color terminals, only the first 8 colors
 * can be redefined (if you are lucky).
 * Note that due to restrictions of most terminal emulators
 * and some curses implementations, this command simply will not
 * restore the original palette entry or request
 * when rubbed out and should generally only be used in
 * \fIbatch-mode\fP \(em typically when loading a color scheme.
 * For the same reasons \(em even though \*(ST tries hard to
 * restore the original palette on exit \(em palette changes may
 * persist after \*(ST terminates on most terminal emulators on Unix.
 * The only emulator which will restore their default palette
 * on exit the author is aware of is \fBxterm\fP(1) and
 * the Linux console driver.
 * You have been warned. Good luck.
 * .IP 4:
 * The column after the last horizontal movement.
 * This is only used by \fBfnkeys.tes\fP and is similar to the Scintilla-internal
 * setting \fBSCI_CHOOSECARETX\fP.
 * Unless most other settings, this is on purpose not restored on rubout,
 * so it "survives" command line replacements.
 * .
 * .IP -1:
 * Type of the last mouse event (\fBread-only\fP).
 * One of the following values will be returned:
 * .RS
 * .  IP 1: 4
 * Some button has been pressed
 * .  IP 2:
 * Some button has been released
 * .  IP 3:
 * Scroll up
 * .  IP 4:
 * Scroll down
 * .RE
 * .IP -2:
 * Coordinates of the mouse pointer relative to the Scintilla view
 * at the time of the last mouse event.
 * This is in pixels or cells depending on the UI.
 * First the Y coordinate is pushed, followed by the X coordinate,
 * allowing you to pass them on directly to the \fBSCI_POSITIONFROMPOINT\fP
 * and similar Scintilla messages using the \fBES\fP command.
 * (\fBread-only\fP)
 * .IP -3:
 * Number of the mouse button involved in the last mouse event, beginning with 1.
 * Can be -1 if the button cannot be determined or is irrelevant.
 * (\fBread-only\fP)
 * .IP -4:
 * Bit mask describing the key modifiers at the time of the last
 * mouse event (\fBread-only\fP).
 * Currently, the following flags are used:
 * .RS
 * .  IP 1: 4
 * Shift key
 * .  IP 2:
 * Control key (CTRL)
 * .  IP 4:
 * Alt key
 * .RE
 */
static void
teco_state_ecommand_properties(teco_machine_main_t *ctx, GError **error)
{
	enum {
		EJ_MOUSE_MODS = -4,
		EJ_MOUSE_BUTTON,
		EJ_MOUSE_COORD,
		EJ_MOUSE_TYPE,

		EJ_USER_INTERFACE = 0,
		EJ_BUFFERS,
		EJ_MEMORY_LIMIT,
		EJ_INIT_COLOR,
		EJ_CARETX
	};

	static teco_int_t caret_x = 0;

	teco_int_t property;
	if (!teco_expressions_pop_num_calc(&property, teco_num_sign, error))
		return;

	if (teco_expressions_args() > 0) {
		/*
		 * Set property
		 */
		teco_int_t value = teco_expressions_pop_num(0);

		switch (property) {
		case EJ_MEMORY_LIMIT:
			if (!teco_memory_set_limit(MAX(0, value), error))
				return;
			break;

		case EJ_INIT_COLOR:
			if (value < 0 || value >= 16) {
				g_set_error(error, TECO_ERROR, TECO_ERROR_FAILED,
				            "Invalid color code %" TECO_INT_FORMAT " "
				            "specified for <EJ>", value);
				return;
			}
			if (!teco_expressions_args()) {
				teco_error_argexpected_set(error, "EJ");
				return;
			}
			teco_interface_init_color((guint)value,
			                          (guint32)teco_expressions_pop_num(0));
			break;

		case EJ_CARETX:
			caret_x = value;
			break;

		default:
			g_set_error(error, TECO_ERROR, TECO_ERROR_FAILED,
			            "Cannot set property %" TECO_INT_FORMAT " "
			            "for <EJ>", property);
			return;
		}

		return;
	}

	/*
	 * Get property
	 */
	switch (property) {
	case EJ_MOUSE_TYPE:
		teco_expressions_push(teco_mouse.type);
		break;
	case EJ_MOUSE_COORD:
		/* can be passed down to @ES/POSITIONFROMPOINT// */
		teco_expressions_push(teco_mouse.y);
		teco_expressions_push(teco_mouse.x);
		break;
	case EJ_MOUSE_BUTTON:
		teco_expressions_push(teco_mouse.button);
		break;
	case EJ_MOUSE_MODS:
		teco_expressions_push(teco_mouse.mods);
		break;

	case EJ_USER_INTERFACE:
		/*
		 * FIXME: Replace INTERFACE_* macros with
		 * teco_interface_id()?
		 */
#ifdef INTERFACE_CURSES
		teco_expressions_push(1);
#elif defined(INTERFACE_GTK)
		teco_expressions_push(2);
#else
#error Missing value for current interface!
#endif
		break;

	case EJ_BUFFERS:
		teco_expressions_push(teco_ring_get_id(teco_ring_last()));
		break;

	case EJ_MEMORY_LIMIT:
		teco_expressions_push(teco_memory_limit);
		break;

	case EJ_CARETX:
		teco_expressions_push(caret_x);
		break;

	default:
		g_set_error(error, TECO_ERROR, TECO_ERROR_FAILED,
		            "Invalid property %" TECO_INT_FORMAT " "
		            "for <EJ>", property);
		return;
	}
}

/*$ "EL" ":EL" EOL
 * 0EL -- Set or get End of Line mode
 * 13,10:EL
 * 1EL
 * 13:EL
 * 2EL
 * 10:EL
 * EL -> 0 | 1 | 2
 * :EL -> 13,10 | 13 | 10
 *
 * Sets or gets the current document's End Of Line (EOL) mode.
 * This is a thin wrapper around Scintilla's
 * \fBSCI_SETEOLMODE\fP and \fBSCI_GETEOLMODE\fP messages but is
 * shorter to type and supports restoring the EOL mode upon rubout.
 * Like the Scintilla message, <EL> does \fBnot\fP change the
 * characters in the current document.
 * If automatic EOL translation is activated (which is the default),
 * \*(ST will however use this information when saving files or
 * writing to external processes.
 *
 * With one argument, the EOL mode is set according to these
 * constants:
 * .IP 0 4
 * Carriage return (ASCII 13), followed by line feed (ASCII 10).
 * This is the default EOL mode on DOS/Windows.
 * .IP 1
 * Carriage return (ASCII 13).
 * The default EOL mode on old Mac OS systems.
 * .IP 2
 * Line feed (ASCII 10).
 * The default EOL mode on POSIX/UNIX systems.
 *
 * In its colon-modified form, the EOL mode is set according
 * to the EOL characters on the expression stack.
 * \*(ST will only pop as many values as are necessary to
 * determine the EOL mode.
 *
 * Without arguments, the current EOL mode is returned.
 * When colon-modified, the current EOL mode's character sequence
 * is pushed onto the expression stack.
 */
static void
teco_state_ecommand_eol(teco_machine_main_t *ctx, GError **error)
{
	if (!teco_expressions_eval(FALSE, error))
		return;

	if (teco_expressions_args() > 0) {
		teco_int_t eol_mode;

		if (teco_machine_main_eval_colon(ctx) > 0) {
			switch (teco_expressions_pop_num(0)) {
			case '\r':
				eol_mode = SC_EOL_CR;
				break;
			case '\n':
				if (!teco_expressions_args()) {
					eol_mode = SC_EOL_LF;
					break;
				}
				if (teco_expressions_pop_num(0) == '\r') {
					eol_mode = SC_EOL_CRLF;
					break;
				}
				/* fall through */
			default:
				g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
				                    "Invalid EOL sequence for <EL>");
				return;
			}
		} else {
			eol_mode = teco_expressions_pop_num(0);
			switch (eol_mode) {
			case SC_EOL_CRLF:
			case SC_EOL_CR:
			case SC_EOL_LF:
				break;
			default:
				g_set_error(error, TECO_ERROR, TECO_ERROR_FAILED,
				            "Invalid EOL mode %" TECO_INT_FORMAT " for <EL>",
				            eol_mode);
				return;
			}
		}

		if (teco_current_doc_must_undo())
			undo__teco_interface_ssm(SCI_SETEOLMODE,
			                         teco_interface_ssm(SCI_GETEOLMODE, 0, 0), 0);
		teco_interface_ssm(SCI_SETEOLMODE, eol_mode, 0);

		/*
		 * While the buffer contents were not changed,
		 * the result of saving the file may differ,
		 * so we still dirtify the buffer.
		 */
		teco_ring_dirtify();
	} else if (teco_machine_main_eval_colon(ctx) > 0) {
		const gchar *eol_seq = teco_eol_get_seq(teco_interface_ssm(SCI_GETEOLMODE, 0, 0));
		teco_expressions_push(eol_seq);
	} else {
		teco_expressions_push(teco_interface_ssm(SCI_GETEOLMODE, 0, 0));
	}
}

static const gchar *
teco_codepage2str(guint codepage)
{
	/*
	 * The multi-byte charsets are excluded, since we don't
	 * support them in SciTECO, even though Scintilla has them.
	 * Contrary to the Scintilla documentation, Gtk supports
	 * most of them.
	 * Those that are supported are tested, so the codepage
	 * mapping should be definitive (although there could be
	 * similar related codepages).
	 */
	switch (codepage) {
	case SC_CP_UTF8:		return "UTF-8";
	case SC_CHARSET_ANSI:
	case SC_CHARSET_DEFAULT:	return "ISO-8859-1"; /* LATIN1 */
	case SC_CHARSET_BALTIC:		return "ISO-8859-13"; /* LATIN7 */
	//case SC_CHARSET_CHINESEBIG5:	return "BIG5";
	case SC_CHARSET_EASTEUROPE:	return "ISO-8859-2"; /* LATIN2 */
	//case SC_CHARSET_GB2312:	return "GB2312";
	case SC_CHARSET_GREEK:		return "ISO-8859-7"; // CP1253???
	//case SC_CHARSET_HANGUL:	return "UHC";
	/* unsure whether this is supported on Gtk */
	case SC_CHARSET_MAC:		return "MAC";
	/* not supported by Gtk */
	case SC_CHARSET_OEM:		return "CP437";
	/*
	 * Apparently, this can be CP1251 on the native Windows
	 * port of Scintilla.
	 */
	case SC_CHARSET_RUSSIAN:	return "KOI8-R";
	case SC_CHARSET_OEM866:		return "CP866";
	case SC_CHARSET_CYRILLIC:	return "CP1251";
	//case SC_CHARSET_SHIFTJIS:	return "SHIFT-JIS";
	//case SC_CHARSET_SYMBOL:
	case SC_CHARSET_TURKISH:	return "ISO-8859-9"; /* LATIN5 */
	//case SC_CHARSET_JOHAB:	return "JOHAB";
	case SC_CHARSET_HEBREW:		return "ISO-8859-8"; // CP1255?
	/*
	 * FIXME: Some arabic codepage is supported by Gtk,
	 * but I am not sure which.
	 */
	case SC_CHARSET_ARABIC:		return "ISO-8859-6"; // CP720, CP1256???
	/* apparently not supported by Gtk */
	case SC_CHARSET_VIETNAMESE:	return "CP1258";
	case SC_CHARSET_THAI:		return "ISO-8859-11";
	case SC_CHARSET_8859_15:	return "ISO-8859-15"; /* LATIN9 */
	}

	return NULL;
}

/*$ "EE" ":EE" encoding codepage charset
 * codepageEE -- Edit current document's encoding (codepage/charset)
 * EE -> codepage
 * codepage:EE
 * :EE -> codepage
 *
 * When called with an argument, it sets the current codepage,
 * otherwise returns it.
 * The following codepages are supported:
 * - 0: ANSI (raw bytes)
 * - 1: ISO-8859-1 (latin1)
 * - 77: Macintosh Latin encoding
 * - 161: ISO-8859-7
 * - 162: ISO-8859-9 (latin5)
 * - 163: CP1258
 * - 177: ISO-8859-8
 * - 178: ISO-8859-6
 * - 186: ISO-8859-13 (latin7)
 * - 204: KOI8-R
 * - 222: ISO-8859-11
 * - 238: ISO-8859-2 (latin2)
 * - 255: CP437
 * - 866: CP866
 * - 1000: ISO-8859-15 (latin9)
 * - 1251: CP1251
 * - 65001: UTF-8
 *
 * Displaying characters in the single-byte (non-UTF-8) codepages might
 * be supported only with the Gtk UI.
 * At least 77, 178, 163 and 255 are not displayed correctly on Gtk.
 * 65001 (UTF-8) is the default for new buffers.
 * 0 (ANSI) should be used when working with raw bytes,
 * but is currently displayed like ISO-8859-1 (latin1).
 *
 * \fBEE\fP does not change the buffer contents itself by default, only
 * how it is displayed and how \*(ST interacts with it.
 * This allows fixing up the codepage if it is not in the default UTF-8
 * or if codepage guessing failed.
 *
 * When colon-modified the \fB:EE\fP command will also additionally convert
 * the current buffer contents into the new code page, preserving the
 * current position (dot).
 * This will fail if the conversion would be lossy.
 * Conversions from and to UTF-8 \fIshould\fP always be successful.
 */
static void
teco_state_ecommand_encoding(teco_machine_main_t *ctx, GError **error)
{
	if (!teco_expressions_eval(FALSE, error))
		return;

	gboolean colon_modified = teco_machine_main_eval_colon(ctx) > 0;

	guint old_cp = teco_interface_get_codepage();

	if (!teco_expressions_args()) {
		/* get current code page */
		teco_expressions_push(old_cp);
		return;
	}

	/*
	 * Set code page
	 */
	teco_int_t new_cp = teco_expressions_pop_num(0);
	if (old_cp == SC_CP_UTF8 && new_cp == SC_CP_UTF8)
		return;

	if (teco_current_doc_must_undo() && teco_undo_enabled) {
		if (old_cp == SC_CP_UTF8) { /* new_cp != SC_CP_UTF8 */
			undo__teco_interface_ssm(SCI_ALLOCATELINECHARACTERINDEX,
			                         SC_LINECHARACTERINDEX_UTF32, 0);
			undo__teco_interface_ssm(SCI_SETCODEPAGE, SC_CP_UTF8, 0);
		} else {
			undo__teco_interface_ssm(SCI_SETCODEPAGE, 0, 0);
			for (gint style = 0; style <= STYLE_LASTPREDEFINED; style++)
				undo__teco_interface_ssm(SCI_STYLESETCHARACTERSET, style, old_cp);
			/*
			 * The index is internally reference-counted and could underflow,
			 * so don't do it more than necessary.
			 */
			if (new_cp == SC_CP_UTF8)
				undo__teco_interface_ssm(SCI_RELEASELINECHARACTERINDEX,
				                         SC_LINECHARACTERINDEX_UTF32, 0);
		}
	}

	teco_int_t dot_glyphs = 0;
	if (colon_modified) {
		sptr_t dot_bytes = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);
		dot_glyphs = teco_interface_bytes2glyphs(dot_bytes);

		/*
		 * Convert buffer to new codepage.
		 *
		 * FIXME: Could be optimized slightly by converting first
		 * before the gap, inserting the converted text and then
		 * converting after the gap.
		 */
		const gchar *to_codepage = teco_codepage2str(new_cp);
		const gchar *from_codepage = teco_codepage2str(old_cp);
		if (!to_codepage || !from_codepage) {
			g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
			                    "Unknown or unsupported codepage/charset");
			return;
		}

		const gchar *buf = (const gchar *)teco_interface_ssm(SCI_GETCHARACTERPOINTER, 0, 0);
		gsize len = teco_interface_ssm(SCI_GETLENGTH, 0, 0);
		g_autofree gchar *converted;
		gsize converted_len;

		/*
		 * This fails if there is no direct translation.
		 * If we'd use g_convert_with_fallback(), it would be tricky to choose
		 * fallback characters that will always work.
		 */
		converted = g_convert(buf, len, to_codepage, from_codepage,
		                      NULL, &converted_len, error);
		if (!converted)
			return;

		teco_interface_ssm(SCI_BEGINUNDOACTION, 0, 0);
		teco_interface_ssm(SCI_CLEARALL, 0, 0);
		teco_interface_ssm(SCI_APPENDTEXT, converted_len, (sptr_t)converted);
		teco_interface_ssm(SCI_ENDUNDOACTION, 0, 0);
		teco_ring_dirtify();

		if (teco_current_doc_must_undo()) {
			undo__teco_interface_ssm(SCI_GOTOPOS, dot_bytes, 0);
			undo__teco_interface_ssm(SCI_UNDO, 0, 0);
		}
	}

	if (new_cp == SC_CP_UTF8) {
		teco_interface_ssm(SCI_SETCODEPAGE, SC_CP_UTF8, 0);
		/*
		 * UTF-8 documents strictly require the line character index.
		 * See teco_view_glyphs2bytes() and teco_view_bytes2glyphs().
		 */
		g_assert(!(teco_interface_ssm(SCI_GETLINECHARACTERINDEX, 0, 0)
						& SC_LINECHARACTERINDEX_UTF32));
		teco_interface_ssm(SCI_ALLOCATELINECHARACTERINDEX,
		                   SC_LINECHARACTERINDEX_UTF32, 0);
	} else {
		/*
		 * The index is NOT released automatically when setting the codepage.
		 * But it is internally reference-counted and could underflow,
		 * so don't do it more than necessary.
		 */
		if (old_cp == SC_CP_UTF8) {
			teco_interface_ssm(SCI_RELEASELINECHARACTERINDEX,
			                   SC_LINECHARACTERINDEX_UTF32, 0);
			g_assert(!(teco_interface_ssm(SCI_GETLINECHARACTERINDEX, 0, 0)
							& SC_LINECHARACTERINDEX_UTF32));
		}

		/*
		 * Configure a single-byte codepage/charset.
		 * This requires setting it on all of the possible styles.
		 * Unfortunately there can theoretically even be 255 (STYLE_MAX) styles.
		 * This is important only for display purposes - other than that
		 * all single-byte encodings are handled the same.
		 *
		 * FIXME: Should we avoid this if new_cp == 0?
		 * It will be used for raw byte handling mostly.
		 */
		if (teco_current_doc_must_undo()) {
			/*
			 * There is a chance the user will see this buffer even if we
			 * are currently in batch mode.
			 */
			for (gint style = 0; style <= STYLE_LASTPREDEFINED; style++)
				teco_interface_ssm(SCI_STYLESETCHARACTERSET, style, new_cp);
		} else {
			/* we must still set it, so that <EE> retrieval works */
			teco_interface_ssm(SCI_STYLESETCHARACTERSET, STYLE_DEFAULT, new_cp);
		}
		/* 0 is used for ALL single-byte encodings */
		teco_interface_ssm(SCI_SETCODEPAGE, 0, 0);
	}

	if (colon_modified)
		/*
		 * Only now, it will be safe to recalculate dot in the new encoding.
		 * If the new codepage is UTF-8, the line character index will be
		 * ready only now.
		 */
		teco_interface_ssm(SCI_GOTOPOS, teco_interface_glyphs2bytes(dot_glyphs), 0);
}

/*$ EO version
 * EO -> major*10000 + minor*100 + micro
 *
 * Return the version of \*(ST encoded into an integer.
 */
static void
teco_state_ecommand_version(teco_machine_main_t *ctx, GError **error)
{
	/*
	 * FIXME: This is inefficient and could be done at build-time.
	 * Or we could have PACKAGE_MAJOR_VERSION, PACKAGE_MINOR_VERSION etc. macros.
	 * But then, who cares?
	 */
	guint major, minor, micro;
	G_GNUC_UNUSED gint rc = sscanf(PACKAGE_VERSION, "%u.%u.%u", &major, &minor, &micro);
	g_assert(rc == 3);
	teco_expressions_push(major*10000 + minor*100 + micro);
}

/*$ "EX" ":EX" exit quit
 * [bool]EX -- Exit program
 * -EX
 * :EX
 *
 * Exits \*(ST, or rather requests program termination
 * at the end of the top-level macro.
 * Therefore instead of exiting immediately which
 * could be annoying in interactive mode, EX will
 * result in program termination only when the command line
 * is terminated.
 * This allows EX to be rubbed out and used in macros.
 * The usual command to exit \*(ST in interactive mode
 * is thus \(lqEX\fB$$\fP\(rq.
 * In batch mode EX will exit the program if control
 * reaches the end of the munged file \(em instead of
 * starting up interactive mode.
 *
 * If any buffer is dirty (modified), EX will yield
 * an error.
 * When specifying <bool> as a success/truth condition
 * boolean, EX will not check whether there are modified
 * buffers and will always succeed.
 * If <bool> is omitted, the sign prefix is implied
 * (1 or -1).
 * In other words \(lq-EX\fB$$\fP\(rq is the usual
 * interactive command sequence to discard all unsaved
 * changes and exit.
 *
 * When colon-modified, <bool> is ignored and EX
 * will instead immediately try to save all modified buffers \(em
 * this can of course be reversed using rubout.
 * Saving all buffers can fail, e.g. if the unnamed file
 * is modified or if there is an IO error.
 * \(lq:EX\fB$$\fP\(rq is nevertheless the usual interactive
 * command sequence to exit while saving all modified
 * buffers.
 *
 * The program termination request is also available in bit 2
 * of the \fBED\fP flags, so \(lqED&2\(rq can be used to
 * check whether EX has been successfully called.
 */
/** @fixme what if changing file after EX? will currently still exit */
static void
teco_state_ecommand_exit(teco_machine_main_t *ctx, GError **error)
{
	if (teco_machine_main_eval_colon(ctx) > 0) {
		if (!teco_ring_save_all_dirty_buffers(error))
			return;
	} else {
		teco_int_t v;
		if (!teco_expressions_pop_num_calc(&v, teco_num_sign, error))
			return;
		guint id;
		if (teco_is_failure(v) && (id = teco_ring_get_first_dirty())) {
			g_set_error(error, TECO_ERROR, TECO_ERROR_FAILED,
			            "Buffer with id %u is dirty", id);
			return;
		}
	}

	teco_undo_int(teco_ed) |= TECO_ED_EXIT;
}

static void
teco_state_macrofile_deprecated(teco_machine_main_t *ctx, GError **error)
{
	teco_interface_msg(TECO_MSG_WARNING,
	                   "<EM> command is deprecated - use <EI> instead");
}

static teco_state_t *
teco_state_ecommand_input(teco_machine_main_t *ctx, gunichar chr, GError **error)
{
	static teco_machine_main_transition_t transitions[] = {
		/*
		 * Simple Transitions
		 */
		['%']  = {&teco_state_epctcommand,
		          .modifier_at = TRUE},
		['B']  = {&teco_state_edit_file,
		          .modifier_at = TRUE},
		['C']  = {&teco_state_execute,
		          .modifier_at = TRUE, .modifier_colon = 1},
		['G']  = {&teco_state_egcommand,
		          .modifier_at = TRUE, .modifier_colon = 1},
		['I']  = {&teco_state_indirect,
		          .modifier_at = TRUE, .modifier_colon = 1},
		/* DEPRECATED: can be repurposed */
		['M']  = {&teco_state_indirect, teco_state_macrofile_deprecated,
		          .modifier_at = TRUE, .modifier_colon = 1},
		['N']  = {&teco_state_glob_pattern,
		          .modifier_at = TRUE, .modifier_colon = 1},
		['S']  = {&teco_state_scintilla_symbols,
		          .modifier_at = TRUE},
		['Q']  = {&teco_state_eqcommand,
		          .modifier_at = TRUE},
		['U']  = {&teco_state_eucommand,
		          .modifier_at = TRUE, .modifier_colon = 1},
		['W']  = {&teco_state_save_file,
		          .modifier_at = TRUE},
		['R']  = {&teco_state_read_file,
		          .modifier_at = TRUE},

		/*
		 * Commands
		 */
		['F']  = {&teco_state_start, teco_state_ecommand_close,
		          .modifier_colon = 1},
		['D']  = {&teco_state_start, teco_state_ecommand_flags},
		['J']  = {&teco_state_start, teco_state_ecommand_properties},
		['L']  = {&teco_state_start, teco_state_ecommand_eol,
		          .modifier_colon = 1},
		['E']  = {&teco_state_start, teco_state_ecommand_encoding,
		          .modifier_colon = 1},
		['O']  = {&teco_state_start, teco_state_ecommand_version},
		['X']  = {&teco_state_start, teco_state_ecommand_exit,
		          .modifier_colon = 1},
	};

	/*
	 * FIXME: Should we return a special syntax error in case of failure?
	 */
	return teco_machine_main_transition_input(ctx, transitions, G_N_ELEMENTS(transitions),
	                                          teco_ascii_toupper(chr), error);
}

TECO_DEFINE_STATE_COMMAND(teco_state_ecommand);

gboolean
teco_state_insert_initial(teco_machine_main_t *ctx, GError **error)
{
	if (ctx->flags.mode > TECO_MODE_NORMAL)
		return TRUE;

	sptr_t pos = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);
	teco_undo_int(teco_ranges[0].from) = teco_interface_bytes2glyphs(pos);
	teco_undo_guint(teco_ranges_count) = 1;

	/*
	 * Current document's encoding determines the behaviour of
	 * string building constructs.
	 */
	teco_machine_stringbuilding_set_codepage(&ctx->expectstring.machine,
	                                         teco_interface_get_codepage());

	if (!teco_expressions_eval(FALSE, error))
		return FALSE;
	guint args = teco_expressions_args();
	if (!args)
		return TRUE;

	if (teco_interface_ssm(SCI_GETCODEPAGE, 0, 0) == SC_CP_UTF8) {
		/* detect possible errors before introducing side effects */
		for (gint i = args; i > 0; i--) {
			teco_int_t chr = teco_expressions_peek_num(i-1);
			if (chr < 0 || !g_unichar_validate(chr)) {
				teco_error_codepoint_set(error, "I");
				return FALSE;
			}
		}
		teco_interface_ssm(SCI_BEGINUNDOACTION, 0, 0);
		for (gint i = args; i > 0; i--) {
			/* 4 bytes should be enough, but we better follow the documentation */
			gchar buf[6];
			gsize len = g_unichar_to_utf8(teco_expressions_peek_num(i-1), buf);
			teco_interface_ssm(SCI_ADDTEXT, len, (sptr_t)buf);
		}
	} else {
		/* everything else is a single-byte encoding */
		for (gint i = args; i > 0; i--) {
			teco_int_t chr = teco_expressions_peek_num(i-1);
			if (chr < 0 || chr > 0xFF) {
				teco_error_codepoint_set(error, "I");
				return FALSE;
			}
		}
		teco_interface_ssm(SCI_BEGINUNDOACTION, 0, 0);
		for (gint i = args; i > 0; i--) {
			gchar chr = (gchar)teco_expressions_peek_num(i-1);
			teco_interface_ssm(SCI_ADDTEXT, 1, (sptr_t)&chr);
		}
	}
	teco_interface_ssm(SCI_ENDUNDOACTION, 0, 0);
	teco_ring_dirtify();

	if (teco_current_doc_must_undo())
		undo__teco_interface_ssm(SCI_UNDO, 0, 0);

	/* This is done only now because it can _theoretically_ fail. */
	for (gint i = 0; i < args; i++)
		teco_expressions_pop_num(0);

	return TRUE;
}

gboolean
teco_state_insert_process(teco_machine_main_t *ctx, const teco_string_t *str,
                          gsize new_chars, GError **error)
{
	g_assert(new_chars > 0);

	teco_interface_ssm(SCI_BEGINUNDOACTION, 0, 0);
	teco_interface_ssm(SCI_ADDTEXT, new_chars,
	                   (sptr_t)(str->data + str->len - new_chars));
	teco_interface_ssm(SCI_ENDUNDOACTION, 0, 0);
	teco_ring_dirtify();

	if (teco_current_doc_must_undo())
		undo__teco_interface_ssm(SCI_UNDO, 0, 0);

	return TRUE;
}

teco_state_t *
teco_state_insert_done(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	if (ctx->flags.mode > TECO_MODE_NORMAL)
		return &teco_state_start;

	sptr_t pos = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);
	teco_undo_int(teco_ranges[0].to) = teco_interface_bytes2glyphs(pos);
	return &teco_state_start;
}

/*
 * NOTE: cannot support VideoTECO's <n>I because
 * beginning and end of strings must be determined
 * syntactically
 */
/*$ I insert
 * [c1,c2,...]I[text]$ -- Insert text with string building characters
 *
 * First inserts characters for all the values
 * on the argument stack (interpreted as codepoints).
 * It does so in the order of the arguments, i.e.
 * <c1> is inserted before <c2>, ecetera.
 * Secondly, the command inserts <text>.
 * In interactive mode, <text> is inserted interactively.
 *
 * Unlike in classic TECO dialects, string building characters are
 * \fBenabled\fP for the \fBI\fP command.
 * When editing \*(ST macros, using the \fBEI\fP command
 * may be better, since it has string building characters
 * disabled.
 */
TECO_DEFINE_STATE_INSERT(teco_state_insert_plain);

static gboolean
teco_state_insert_indent_initial(teco_machine_main_t *ctx, GError **error)
{
	if (ctx->flags.mode > TECO_MODE_NORMAL)
		return TRUE;

	if (!teco_state_insert_initial(ctx, error))
		return FALSE;

	teco_interface_ssm(SCI_BEGINUNDOACTION, 0, 0);
	if (teco_interface_ssm(SCI_GETUSETABS, 0, 0)) {
		teco_interface_ssm(SCI_ADDTEXT, 1, (sptr_t)"\t");
	} else {
		gint len = teco_interface_ssm(SCI_GETTABWIDTH, 0, 0);

		len -= teco_interface_ssm(SCI_GETCOLUMN,
		                          teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0), 0) % len;

		gchar space = ' ';
		while (len-- > 0)
			teco_interface_ssm(SCI_ADDTEXT, 1, (sptr_t)&space);
	}
	teco_interface_ssm(SCI_ENDUNDOACTION, 0, 0);
	teco_ring_dirtify();

	if (teco_current_doc_must_undo())
		undo__teco_interface_ssm(SCI_UNDO, 0, 0);

	return TRUE;
}

/*
 * Alternatives: ^i, ^I, <CTRL/I>, <TAB>
 */
/*$ ^I indent
 * [char,...]^I[text]$ -- Insert with leading indention
 *
 * ^I (usually typed using the Tab key), first inserts
 * all the chars on the stack into the buffer, then indention
 * characters (one tab or multiple spaces) and eventually
 * the optional <text> is inserted interactively.
 * It is thus a derivate of the \fBI\fP (insertion) command.
 *
 * \*(ST uses Scintilla settings to determine the indention
 * characters.
 * If tab use is enabled with the \fBSCI_SETUSETABS\fP message,
 * a single tab character is inserted.
 * Tab use is enabled by default.
 * Otherwise, a number of spaces is inserted up to the
 * next tab stop so that the command's <text> argument
 * is inserted at the beginning of the next tab stop.
 * The size of the tab stops is configured by the
 * \fBSCI_SETTABWIDTH\fP Scintilla message (8 by default).
 * In combination with \*(ST's use of the tab key as an
 * immediate editing command for all insertions, this
 * implements support for different insertion styles.
 * The Scintilla settings apply to the current Scintilla
 * document and are thus local to the currently edited
 * buffer or Q-Register.
 *
 * However for the same reason, the ^I command is not
 * fully compatible with classic TECO which \fIalways\fP
 * inserts a single tab character and should not be used
 * for the purpose of inserting single tabs in generic
 * macros.
 * To insert a single tab character reliably, the idioms
 * \(lq9I$\(rq or \(lqI^I$\(rq may be used.
 *
 * Like the I command, ^I has string building characters
 * \fBenabled\fP.
 */
TECO_DEFINE_STATE_INSERT(teco_state_insert_indent,
	.initial_cb = (teco_state_initial_cb_t)teco_state_insert_indent_initial
);
