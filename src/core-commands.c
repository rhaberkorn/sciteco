/*
 * Copyright (C) 2012-2022 Robin Haberkorn
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
#include "qreg-commands.h"
#include "goto-commands.h"
#include "core-commands.h"

static teco_state_t *teco_state_control_input(teco_machine_main_t *ctx, gchar chr, GError **error);

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
 */
static void
teco_state_start_dot(teco_machine_main_t *ctx, GError **error)
{
	if (!teco_expressions_eval(FALSE, error))
		return;
	teco_expressions_push(teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0));
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
	teco_expressions_push(teco_interface_ssm(SCI_GETLENGTH, 0, 0));
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
	teco_expressions_push(teco_interface_ssm(SCI_GETLENGTH, 0, 0));
}

/*$ "\\"
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
		teco_int_t value;

		if (!teco_expressions_pop_num_calc(&value, 0, error))
			return;

		gchar buffer[TECO_EXPRESSIONS_FORMAT_LEN];
		gchar *str = teco_expressions_format(buffer, value);
		g_assert(*str != '\0');

		teco_interface_ssm(SCI_BEGINUNDOACTION, 0, 0);
		teco_interface_ssm(SCI_ADDTEXT, strlen(str), (sptr_t)str);
		teco_interface_ssm(SCI_ENDUNDOACTION, 0, 0);
		teco_ring_dirtify();

		if (teco_current_doc_must_undo())
			undo__teco_interface_ssm(SCI_UNDO, 0, 0);
	} else {
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
			if (c >= '0' && c <= '0' + MIN(teco_radix, 10) - 1)
				v = (v*teco_radix) + (c - '0');
			else if (c >= 'A' &&
				 c <= 'A' + MIN(teco_radix - 10, 26) - 1)
				v = (v*teco_radix) + 10 + (c - 'A');
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
	if (!teco_expressions_eval(FALSE, error) ||
	    !teco_expressions_pop_num_calc(&lctx.counter, -1, error))
		return;
	lctx.pass_through = teco_machine_main_eval_colon(ctx);

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
			teco_undo_guint(ctx->__flags);
		ctx->mode = TECO_MODE_PARSE_ONLY_LOOP;
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

	teco_loop_context_t *lctx = &g_array_index(teco_loop_stack, teco_loop_context_t, teco_loop_stack->len-1);
	gboolean colon_modified = teco_machine_main_eval_colon(ctx);

	/*
	 * Colon-modified loop ends can be used to
	 * aggregate values on the stack.
	 * A non-colon modified ">" behaves like ":>"
	 * for pass-through loop starts, though.
	 */
	if (!lctx->pass_through) {
		if (colon_modified) {
			if (!teco_expressions_eval(FALSE, error))
				return;
			teco_expressions_push_op(TECO_OP_NEW);
		} else if (!teco_expressions_discard_args(error)) {
			return;
		}
	}

	if (lctx->counter == 1) {
		/* this was the last loop iteration */
		if (!lctx->pass_through &&
		    !teco_expressions_brace_close(error))
			return;
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

/*$ ";" break
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
		                    "<;> only allowed in iterations");
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
	if (teco_machine_main_eval_colon(ctx))
		rc = ~rc;

	if (teco_is_success(rc))
		return;

	teco_loop_context_t lctx = g_array_index(teco_loop_stack, teco_loop_context_t, teco_loop_stack->len-1);
	g_array_remove_index(teco_loop_stack, teco_loop_stack->len-1);

	if (!teco_expressions_discard_args(error))
		return;
	if (!lctx.pass_through &&
	    !teco_expressions_brace_close(error))
		return;

	undo__insert_val__teco_loop_stack(teco_loop_stack->len, lctx);

	/* skip to end of loop */
	if (ctx->parent.must_undo)
		teco_undo_guint(ctx->__flags);
	ctx->mode = TECO_MODE_PARSE_ONLY_LOOP;
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

/*$ J jump
 * [position]J -- Go to position in buffer
 * [position]:J -> Success|Failure
 *
 * Sets dot to <position>.
 * If <position> is omitted, 0 is implied and \(lqJ\(rq will
 * go to the beginning of the buffer.
 *
 * If <position> is outside the range of the buffer, the
 * command yields an error.
 * If colon-modified, the command will instead return a
 * condition boolean signalling whether the position could
 * be changed or not.
 */
static void
teco_state_start_jump(teco_machine_main_t *ctx, GError **error)
{
	teco_int_t v;

	if (!teco_expressions_pop_num_calc(&v, 0, error))
		return;

	if (teco_validate_pos(v)) {
		if (teco_current_doc_must_undo())
			undo__teco_interface_ssm(SCI_SETEMPTYSELECTION,
			                         teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0), 0);
		teco_interface_ssm(SCI_SETEMPTYSELECTION, v, 0);

		if (teco_machine_main_eval_colon(ctx))
			teco_expressions_push(TECO_SUCCESS);
	} else if (teco_machine_main_eval_colon(ctx)) {
		teco_expressions_push(TECO_FAILURE);
	} else {
		teco_error_move_set(error, "J");
		return;
	}
}

static teco_bool_t
teco_move_chars(teco_int_t n)
{
	sptr_t pos = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);

	if (!teco_validate_pos(pos + n))
		return TECO_FAILURE;

	teco_interface_ssm(SCI_SETEMPTYSELECTION, pos + n, 0);
	if (teco_current_doc_must_undo())
		undo__teco_interface_ssm(SCI_SETEMPTYSELECTION, pos, 0);

	return TECO_SUCCESS;
}

/*$ C move
 * [n]C -- Move dot <n> characters
 * -C
 * [n]:C -> Success|Failure
 *
 * Adds <n> to dot. 1 or -1 is implied if <n> is omitted.
 * Fails if <n> would move dot off-page.
 * The colon modifier results in a success-boolean being
 * returned instead.
 */
static void
teco_state_start_move(teco_machine_main_t *ctx, GError **error)
{
	teco_int_t v;

	if (!teco_expressions_pop_num_calc(&v, teco_num_sign, error))
		return;

	teco_bool_t rc = teco_move_chars(v);
	if (teco_machine_main_eval_colon(ctx)) {
		teco_expressions_push(rc);
	} else if (teco_is_failure(rc)) {
		teco_error_move_set(error, "C");
		return;
	}
}

/*$ R reverse
 * [n]R -- Move dot <n> characters backwards
 * -R
 * [n]:R -> Success|Failure
 *
 * Subtracts <n> from dot.
 * It is equivalent to \(lq-nC\(rq.
 */
static void
teco_state_start_reverse(teco_machine_main_t *ctx, GError **error)
{
	teco_int_t v;

	if (!teco_expressions_pop_num_calc(&v, teco_num_sign, error))
		return;

	teco_bool_t rc = teco_move_chars(-v);
	if (teco_machine_main_eval_colon(ctx)) {
		teco_expressions_push(rc);
	} else if (teco_is_failure(rc)) {
		teco_error_move_set(error, "R");
		return;
	}
}

static teco_bool_t
teco_move_lines(teco_int_t n)
{
	sptr_t pos = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);
	sptr_t line = teco_interface_ssm(SCI_LINEFROMPOSITION, pos, 0) + n;

	if (!teco_validate_line(line))
		return TECO_FAILURE;

	/* avoids scrolling caret (expensive operation) */
	teco_interface_ssm(SCI_SETEMPTYSELECTION,
	                   teco_interface_ssm(SCI_POSITIONFROMLINE, line, 0), 0);
	if (teco_current_doc_must_undo())
		undo__teco_interface_ssm(SCI_SETEMPTYSELECTION, pos, 0);

	return TECO_SUCCESS;
}

/*$ L line
 * [n]L -- Move dot <n> lines forwards
 * -L
 * [n]:L -> Success|Failure
 *
 * Move dot to the beginning of the line specified
 * relatively to the current line.
 * Therefore a value of 0 for <n> goes to the
 * beginning of the current line, 1 will go to the
 * next line, -1 to the previous line etc.
 * If <n> is omitted, 1 or -1 is implied depending on
 * the sign prefix.
 *
 * If <n> would move dot off-page, the command yields
 * an error.
 * The colon-modifer results in a condition boolean
 * being returned instead.
 */
static void
teco_state_start_line(teco_machine_main_t *ctx, GError **error)
{
	teco_int_t v;

	if (!teco_expressions_pop_num_calc(&v, teco_num_sign, error))
		return;

	teco_bool_t rc = teco_move_lines(v);
	if (teco_machine_main_eval_colon(ctx)) {
		teco_expressions_push(rc);
	} else if (teco_is_failure(rc)) {
		teco_error_move_set(error, "L");
		return;
	}
}

/*$ B backwards
 * [n]B -- Move dot <n> lines backwards
 * -B
 * [n]:B -> Success|Failure
 *
 * Move dot to the beginning of the line <n>
 * lines before the current one.
 * It is equivalent to \(lq-nL\(rq.
 */
static void
teco_state_start_back(teco_machine_main_t *ctx, GError **error)
{
	teco_int_t v;

	if (!teco_expressions_pop_num_calc(&v, teco_num_sign, error))
		return;

	teco_bool_t rc = teco_move_lines(-v);
	if (teco_machine_main_eval_colon(ctx)) {
		teco_expressions_push(rc);
	} else if (teco_is_failure(rc)) {
		teco_error_move_set(error, "B");
		return;
	}
}

/*$ W word
 * [n]W -- Move dot by words
 * -W
 * [n]:W -> Success|Failure
 *
 * Move dot <n> words forward.
 *   - If <n> is positive, dot is positioned at the beginning
 *     of the word <n> words after the current one.
 *   - If <n> is negative, dot is positioned at the end
 *     of the word <n> words before the current one.
 *   - If <n> is zero, dot is not moved.
 *
 * \(lqW\(rq uses Scintilla's definition of a word as
 * configurable using the
 * .B SCI_SETWORDCHARS
 * message.
 *
 * Otherwise, the command's behaviour is analogous to
 * the \(lqC\(rq command.
 */
static void
teco_state_start_word(teco_machine_main_t *ctx, GError **error)
{
	teco_int_t v;
	if (!teco_expressions_pop_num_calc(&v, teco_num_sign, error))
		return;
	sptr_t pos = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);

	/*
	 * FIXME: would be nice to do this with constant amount of
	 * editor messages. E.g. by using custom algorithm accessing
	 * the internal document buffer.
	 */
	unsigned int msg = SCI_WORDRIGHTEND;
	if (v < 0) {
		v *= -1;
		msg = SCI_WORDLEFTEND;
	}
	while (v--) {
		sptr_t pos = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);
		teco_interface_ssm(msg, 0, 0);
		if (pos == teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0))
			break;
	}
	if (v < 0) {
		if (teco_current_doc_must_undo())
			undo__teco_interface_ssm(SCI_SETEMPTYSELECTION, pos, 0);
		if (teco_machine_main_eval_colon(ctx))
			teco_expressions_push(TECO_SUCCESS);
	} else {
		teco_interface_ssm(SCI_SETEMPTYSELECTION, pos, 0);
		if (!teco_machine_main_eval_colon(ctx)) {
			teco_error_move_set(error, "W");
			return;
		}
		teco_expressions_push(TECO_FAILURE);
	}
}

static teco_bool_t
teco_delete_words(teco_int_t n)
{
	sptr_t pos, size;

	if (!n)
		return TECO_SUCCESS;

	pos = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);
	size = teco_interface_ssm(SCI_GETLENGTH, 0, 0);
	teco_interface_ssm(SCI_BEGINUNDOACTION, 0, 0);
	/*
	 * FIXME: would be nice to do this with constant amount of
	 * editor messages. E.g. by using custom algorithm accessing
	 * the internal document buffer.
	 */
	if (n > 0) {
		while (n--) {
			sptr_t size = teco_interface_ssm(SCI_GETLENGTH, 0, 0);
			teco_interface_ssm(SCI_DELWORDRIGHTEND, 0, 0);
			if (size == teco_interface_ssm(SCI_GETLENGTH, 0, 0))
				break;
		}
	} else {
		n *= -1;
		while (n--) {
			sptr_t pos = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);
			//teco_interface_ssm(SCI_DELWORDLEFTEND, 0, 0);
			teco_interface_ssm(SCI_WORDLEFTEND, 0, 0);
			if (pos == teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0))
				break;
			teco_interface_ssm(SCI_DELWORDRIGHTEND, 0, 0);
		}
	}
	teco_interface_ssm(SCI_ENDUNDOACTION, 0, 0);

	if (n >= 0) {
		if (size != teco_interface_ssm(SCI_GETLENGTH, 0, 0)) {
			teco_interface_ssm(SCI_UNDO, 0, 0);
			teco_interface_ssm(SCI_SETEMPTYSELECTION, pos, 0);
		}
		return TECO_FAILURE;
	}
	g_assert(size != teco_interface_ssm(SCI_GETLENGTH, 0, 0));

	if (teco_current_doc_must_undo()) {
		undo__teco_interface_ssm(SCI_SETEMPTYSELECTION, pos, 0);
		undo__teco_interface_ssm(SCI_UNDO, 0, 0);
	}
	teco_ring_dirtify();

	return TECO_SUCCESS;
}

/*$ V
 * [n]V -- Delete words forward
 * -V
 * [n]:V -> Success|Failure
 *
 * Deletes the next <n> words until the end of the
 * n'th word after the current one.
 * If <n> is negative, deletes up to end of the
 * n'th word before the current one.
 * If <n> is omitted, 1 or -1 is implied depending on the
 * sign prefix.
 *
 * It uses Scintilla's definition of a word as configurable
 * using the
 * .B SCI_SETWORDCHARS
 * message.
 *
 * If the words to delete extend beyond the range of the
 * buffer, the command yields an error.
 * If colon-modified it instead returns a condition code.
 */
static void
teco_state_start_delete_words(teco_machine_main_t *ctx, GError **error)
{
	teco_int_t v;

	if (!teco_expressions_pop_num_calc(&v, teco_num_sign, error))
		return;

	teco_bool_t rc = teco_delete_words(v);
	if (teco_machine_main_eval_colon(ctx)) {
		teco_expressions_push(rc);
	} else if (teco_is_failure(rc)) {
		teco_error_words_set(error, "V");
		return;
	}
}

/*$ Y
 * [n]Y -- Delete word backwards
 * -Y
 * [n]:Y -> Success|Failure
 *
 * Delete <n> words backward.
 * <n>Y is equivalent to \(lq-nV\(rq.
 */
static void
teco_state_start_delete_words_back(teco_machine_main_t *ctx, GError **error)
{
	teco_int_t v;

	if (!teco_expressions_pop_num_calc(&v, teco_num_sign, error))
		return;

	teco_bool_t rc = teco_delete_words(-v);
	if (teco_machine_main_eval_colon(ctx)) {
		teco_expressions_push(rc);
	} else if (teco_is_failure(rc)) {
		teco_error_words_set(error, "Y");
		return;
	}
}

/*$ "=" print
 * <n>= -- Show value as message
 *
 * Shows integer <n> as a message in the message line and/or
 * on the console.
 * It is currently always formatted as a decimal integer and
 * shown with the user-message severity.
 * The command fails if <n> is not given.
 */
/**
 * @todo perhaps care about current radix
 * @todo colon-modifier to suppress line-break on console?
 */
static void
teco_state_start_print(teco_machine_main_t *ctx, GError **error)
{
	if (!teco_expressions_eval(FALSE, error))
		return;
	if (!teco_expressions_args()) {
		teco_error_argexpected_set(error, "=");
		return;
	}
	teco_int_t v;
	if (!teco_expressions_pop_num_calc(&v, teco_num_sign, error))
		return;
	teco_interface_msg(TECO_MSG_USER, "%" TECO_INT_FORMAT, v);
}

static gboolean
teco_state_start_kill(teco_machine_main_t *ctx, const gchar *cmd, gboolean by_lines, GError **error)
{
	teco_bool_t rc;
	teco_int_t from, len;

	if (!teco_expressions_eval(FALSE, error))
		return FALSE;

	if (teco_expressions_args() <= 1) {
		from = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);
		if (by_lines) {
			teco_int_t line;
			if (!teco_expressions_pop_num_calc(&line, teco_num_sign, error))
				return FALSE;
			line += teco_interface_ssm(SCI_LINEFROMPOSITION, from, 0);
			len = teco_interface_ssm(SCI_POSITIONFROMLINE, line, 0) - from;
			rc = teco_bool(teco_validate_line(line));
		} else {
			if (!teco_expressions_pop_num_calc(&len, teco_num_sign, error))
				return FALSE;
			rc = teco_bool(teco_validate_pos(from + len));
		}
		if (len < 0) {
			len *= -1;
			from -= len;
		}
	} else {
		teco_int_t to = teco_expressions_pop_num(0);
		from = teco_expressions_pop_num(0);
		len = to - from;
		rc = teco_bool(len >= 0 && teco_validate_pos(from) &&
		                           teco_validate_pos(to));
	}

	if (teco_machine_main_eval_colon(ctx)) {
		teco_expressions_push(rc);
	} else if (teco_is_failure(rc)) {
		teco_error_range_set(error, cmd);
		return FALSE;
	}

	if (len == 0 || teco_is_failure(rc))
		return TRUE;

	if (teco_current_doc_must_undo()) {
		sptr_t pos = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);
		undo__teco_interface_ssm(SCI_SETEMPTYSELECTION, pos, 0);
		undo__teco_interface_ssm(SCI_UNDO, 0, 0);
	}

	/*
	 * Should always generate an undo action.
	 */
	teco_interface_ssm(SCI_BEGINUNDOACTION, 0, 0);
	teco_interface_ssm(SCI_DELETERANGE, from, len);
	teco_interface_ssm(SCI_ENDUNDOACTION, 0, 0);
	teco_ring_dirtify();

	return TRUE;
}

/*$ K kill
 * [n]K -- Kill lines
 * -K
 * from,to K
 * [n]:K -> Success|Failure
 * from,to:K -> Success|Failure
 *
 * Deletes characters up to the beginning of the
 * line <n> lines after or before the current one.
 * If <n> is 0, \(lqK\(rq will delete up to the beginning
 * of the current line.
 * If <n> is omitted, the sign prefix will be implied.
 * So to delete the entire line regardless of the position
 * in it, one can use \(lq0KK\(rq.
 *
 * If the deletion is beyond the buffer's range, the command
 * will yield an error unless it has been colon-modified
 * so it returns a condition code.
 *
 * If two arguments <from> and <to> are available, the
 * command is synonymous to <from>,<to>D.
 */
static void
teco_state_start_kill_lines(teco_machine_main_t *ctx, GError **error)
{
	teco_state_start_kill(ctx, "K", TRUE, error);
}

/*$ D delete
 * [n]D -- Delete characters
 * -D
 * from,to D
 * [n]:D -> Success|Failure
 * from,to:D -> Success|Failure
 *
 * If <n> is positive, the next <n> characters (up to and
 * character .+<n>) are deleted.
 * If <n> is negative, the previous <n> characters are
 * deleted.
 * If <n> is omitted, the sign prefix will be implied.
 *
 * If two arguments can be popped from the stack, the
 * command will delete the characters with absolute
 * position <from> up to <to> from the current buffer.
 *
 * If the character range to delete is beyond the buffer's
 * range, the command will yield an error unless it has
 * been colon-modified so it returns a condition code
 * instead.
 */
static void
teco_state_start_delete_chars(teco_machine_main_t *ctx, GError **error)
{
	teco_state_start_kill(ctx, "D", FALSE, error);
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
 *   - If <n> is 0, return the <code> of the character
 *     pointed to by dot.
 *   - If <n> is 1, return the <code> of the character
 *     immediately after dot.
 *   - If <n> is -1, return the <code> of the character
 *     immediately preceding dot, ecetera.
 *   - If <n> is omitted, the sign prefix is implied.
 *
 * If the position of the queried character is off-page,
 * the command will yield an error.
 */
/** @todo does Scintilla really return code points??? */
static void
teco_state_start_get(teco_machine_main_t *ctx, GError **error)
{
	teco_int_t v;
	if (!teco_expressions_pop_num_calc(&v, teco_num_sign, error))
		return;
	v += teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);
	/*
	 * NOTE: We cannot use teco_validate_pos() here since
	 * the end of the buffer is not a valid position for <A>.
	 */
	if (v < 0 || v >= teco_interface_ssm(SCI_GETLENGTH, 0, 0)) {
		teco_error_range_set(error, "A");
		return;
	}
	teco_expressions_push(teco_interface_ssm(SCI_GETCHARAT, v, 0));
}

static teco_state_t *
teco_state_start_input(teco_machine_main_t *ctx, gchar chr, GError **error)
{
	static teco_machine_main_transition_t transitions[] = {
		/*
		 * Simple transitions
		 */
		['$']  = {&teco_state_escape},
		['!']  = {&teco_state_label},
		['O']  = {&teco_state_goto},
		['^']  = {&teco_state_control},
		['F']  = {&teco_state_fcommand},
		['"']  = {&teco_state_condcommand},
		['E']  = {&teco_state_ecommand},
		['I']  = {&teco_state_insert_building},
		['?']  = {&teco_state_help},
		['S']  = {&teco_state_search},
		['N']  = {&teco_state_search_all},

		['[']  = {&teco_state_pushqreg},
		[']']  = {&teco_state_popqreg},
		['G']  = {&teco_state_getqregstring},
		['Q']  = {&teco_state_queryqreg},
		['U']  = {&teco_state_setqreginteger},
		['%']  = {&teco_state_increaseqreg},
		['M']  = {&teco_state_macro},
		['X']  = {&teco_state_copytoqreg},

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
		['<']  = {&teco_state_start, teco_state_start_loop_open},
		['>']  = {&teco_state_start, teco_state_start_loop_close},
		[';']  = {&teco_state_start, teco_state_start_break},

		/*
		 * Command-line Editing
		 */
		['{']  = {&teco_state_start, teco_state_start_cmdline_push},
		['}']  = {&teco_state_start, teco_state_start_cmdline_pop},

		/*
		 * Commands
		 */
		['J']  = {&teco_state_start, teco_state_start_jump},
		['C']  = {&teco_state_start, teco_state_start_move},
		['R']  = {&teco_state_start, teco_state_start_reverse},
		['L']  = {&teco_state_start, teco_state_start_line},
		['B']  = {&teco_state_start, teco_state_start_back},
		['W']  = {&teco_state_start, teco_state_start_word},
		['V']  = {&teco_state_start, teco_state_start_delete_words},
		['Y']  = {&teco_state_start, teco_state_start_delete_words_back},
		['=']  = {&teco_state_start, teco_state_start_print},
		['K']  = {&teco_state_start, teco_state_start_kill_lines},
		['D']  = {&teco_state_start, teco_state_start_delete_chars},
		['A']  = {&teco_state_start, teco_state_start_get}
	};

	switch (chr) {
	/*
	 * No-ops:
	 * These are explicitly not handled in teco_state_control,
	 * so that we can potentially reuse the upcaret notations like ^J.
	 */
	case ' ':
	case '\f':
	case '\r':
	case '\n':
	case '\v':
		return &teco_state_start;

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
		if (ctx->mode == TECO_MODE_NORMAL)
			teco_expressions_add_digit(chr);
		return &teco_state_start;

	case '*':
		/*
		 * Special save last commandline command
		 *
		 * FIXME: Maybe, there should be a special teco_state_t
		 * for beginnings of command-lines?
		 * It could also be used for a corresponding FNMACRO mask.
		 */
		if (teco_cmdline.effective_len == 1 && teco_cmdline.str.data[0] == '*')
			return &teco_state_save_cmdline;
		break;

	case '<':
		if (ctx->mode != TECO_MODE_PARSE_ONLY_LOOP)
			break;
		if (ctx->parent.must_undo)
			teco_undo_gint(ctx->nest_level);
		ctx->nest_level++;
		return &teco_state_start;

	case '>':
		if (ctx->mode != TECO_MODE_PARSE_ONLY_LOOP)
			break;
		if (!ctx->nest_level) {
			if (ctx->parent.must_undo)
				teco_undo_guint(ctx->__flags);
			ctx->mode = TECO_MODE_NORMAL;
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
		if (ctx->parent.must_undo)
			teco_undo_guint(ctx->__flags);
		if (ctx->mode == TECO_MODE_PARSE_ONLY_COND && !ctx->nest_level)
			ctx->mode = TECO_MODE_NORMAL;
		else if (ctx->mode == TECO_MODE_NORMAL)
			/* skip to end of conditional; skip ELSE-part */
			ctx->mode = TECO_MODE_PARSE_ONLY_COND;
		return &teco_state_start;

	case '\'':
		switch (ctx->mode) {
		case TECO_MODE_PARSE_ONLY_COND:
		case TECO_MODE_PARSE_ONLY_COND_FORCE:
			if (!ctx->nest_level) {
				if (ctx->parent.must_undo)
					teco_undo_guint(ctx->__flags);
				ctx->mode = TECO_MODE_NORMAL;
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
	 * Modifiers
	 */
	case '@':
		/*
		 * @ modifier has syntactic significance, so set it even
		 * in PARSE_ONLY* modes
		 */
		if (ctx->parent.must_undo)
			teco_undo_guint(ctx->__flags);
		ctx->modifier_at = TRUE;
		return &teco_state_start;

	case ':':
		if (ctx->mode == TECO_MODE_NORMAL) {
			if (ctx->parent.must_undo)
				teco_undo_guint(ctx->__flags);
			ctx->modifier_colon = TRUE;
		}
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

TECO_DEFINE_STATE_CASEINSENSITIVE(teco_state_start,
	.end_of_macro_cb = NULL, /* Allowed at the end of a macro! */
	.is_start = TRUE,
	.fnmacro_mask = TECO_FNMACRO_MASK_START
);

/*$ F<
 * F< -- Go to loop start or jump to beginning of macro
 *
 * Immediately jumps to the current loop's start.
 * Also works from inside conditionals.
 *
 * Outside of loops \(em or in a macro without
 * a loop \(em this jumps to the beginning of the macro.
 */
static void
teco_state_fcommand_loop_start(teco_machine_main_t *ctx, GError **error)
{
	/* FIXME: what if in brackets? */
	if (!teco_expressions_discard_args(error))
		return;

	ctx->macro_pc = teco_loop_stack->len > ctx->loop_stack_fp
	              ? g_array_index(teco_loop_stack, teco_loop_context_t, teco_loop_stack->len-1).pc : -1;
}

/*$ F> continue
 * F> -- Go to loop end
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
 * When colon-modified, \fB:F>\fP behaves like \fB:>\fP
 * and allows numbers to be aggregated on the stack.
 *
 * Calling \fBF>\fP outside of a loop at the current
 * macro invocation level will throw an error.
 */
static void
teco_state_fcommand_loop_end(teco_machine_main_t *ctx, GError **error)
{
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
			teco_undo_guint(ctx->__flags);
		ctx->mode = TECO_MODE_PARSE_ONLY_LOOP;
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
		teco_undo_guint(ctx->__flags);
	ctx->mode = TECO_MODE_PARSE_ONLY_COND_FORCE;
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
		teco_undo_guint(ctx->__flags);
	ctx->mode = TECO_MODE_PARSE_ONLY_COND;
}

static teco_state_t *
teco_state_fcommand_input(teco_machine_main_t *ctx, gchar chr, GError **error)
{
	static teco_machine_main_transition_t transitions[] = {
		/*
		 * Simple transitions
		 */
		['K']  = {&teco_state_search_kill},
		['D']  = {&teco_state_search_delete},
		['S']  = {&teco_state_replace},
		['R']  = {&teco_state_replace_default},
		['G']  = {&teco_state_changedir},

		/*
		 * Loop Flow Control
		 */
		['<']  = {&teco_state_start, teco_state_fcommand_loop_start},
		['>']  = {&teco_state_start, teco_state_fcommand_loop_end},

		/*
		 * Conditional Flow Control
		 */
		['\''] = {&teco_state_start, teco_state_fcommand_cond_end},
		['|']  = {&teco_state_start, teco_state_fcommand_cond_else}
	};

	return teco_machine_main_transition_input(ctx, transitions, G_N_ELEMENTS(transitions),
	                                          teco_ascii_toupper(chr), error);
}

TECO_DEFINE_STATE_CASEINSENSITIVE(teco_state_fcommand);

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
	if (ctx->mode > TECO_MODE_NORMAL)
		return &teco_state_start;

	g_autofree gchar *dir = teco_file_expand_path(str->data);
	if (!*dir) {
		teco_qreg_t *qreg = teco_qreg_table_find(&teco_qreg_table_globals, "$HOME", 5);
		g_assert(qreg != NULL);
		teco_string_t home;
		if (!qreg->vtable->get_string(qreg, &home.data, &home.len, error))
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
teco_state_condcommand_input(teco_machine_main_t *ctx, gchar chr, GError **error)
{
	teco_int_t value = 0;
	gboolean result = TRUE;

	switch (ctx->mode) {
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
		if (!teco_expressions_pop_num_calc(&value, 0, error))
			return NULL;
		break;

	default:
		break;
	}

	switch (teco_ascii_toupper(chr)) {
	case '~':
		if (ctx->mode == TECO_MODE_NORMAL)
			result = !teco_expressions_args();
		break;
	case 'A':
		if (ctx->mode == TECO_MODE_NORMAL)
			result = g_ascii_isalpha((gchar)value);
		break;
	case 'C':
		if (ctx->mode == TECO_MODE_NORMAL)
			result = g_ascii_isalnum((gchar)value) ||
			         value == '.' || value == '$' || value == '_';
		break;
	case 'D':
		if (ctx->mode == TECO_MODE_NORMAL)
			result = g_ascii_isdigit((gchar)value);
		break;
	case 'I':
		if (ctx->mode == TECO_MODE_NORMAL)
			result = G_IS_DIR_SEPARATOR((gchar)value);
		break;
	case 'S':
	case 'T':
		if (ctx->mode == TECO_MODE_NORMAL)
			result = teco_is_success(value);
		break;
	case 'F':
	case 'U':
		if (ctx->mode == TECO_MODE_NORMAL)
			result = teco_is_failure(value);
		break;
	case 'E':
	case '=':
		if (ctx->mode == TECO_MODE_NORMAL)
			result = value == 0;
		break;
	case 'G':
	case '>':
		if (ctx->mode == TECO_MODE_NORMAL)
			result = value > 0;
		break;
	case 'L':
	case '<':
		if (ctx->mode == TECO_MODE_NORMAL)
			result = value < 0;
		break;
	case 'N':
		if (ctx->mode == TECO_MODE_NORMAL)
			result = value != 0;
		break;
	case 'R':
		if (ctx->mode == TECO_MODE_NORMAL)
			result = g_ascii_isalnum((gchar)value);
		break;
	case 'V':
		if (ctx->mode == TECO_MODE_NORMAL)
			result = g_ascii_islower((gchar)value);
		break;
	case 'W':
		if (ctx->mode == TECO_MODE_NORMAL)
			result = g_ascii_isupper((gchar)value);
		break;
	default:
		g_set_error(error, TECO_ERROR, TECO_ERROR_FAILED,
		            "Invalid conditional type \"%c\"", chr);
		return NULL;
	}

	if (!result) {
		/* skip to ELSE-part or end of conditional */
		if (ctx->parent.must_undo)
			teco_undo_guint(ctx->__flags);
		ctx->mode = TECO_MODE_PARSE_ONLY_COND;
	}

	return &teco_state_start;
}

TECO_DEFINE_STATE_CASEINSENSITIVE(teco_state_condcommand);

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
	teco_int_t v;

	if (!teco_expressions_args()) {
		teco_error_argexpected_set(error, "^_");
		return;
	}
	if (!teco_expressions_pop_num_calc(&v, 0, error))
		return;
	teco_expressions_push(~v);
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

/*$ ^C exit
 * ^C -- Exit program immediately
 *
 * Lets the top-level macro return immediately
 * regardless of the current macro invocation frame.
 * This command is only allowed in batch mode,
 * so it is not invoked accidentally when using
 * the CTRL+C immediate editing command to
 * interrupt long running operations.
 * When using \fB^C\fP in a munged file,
 * interactive mode is never started, so it behaves
 * effectively just like \(lq-EX\fB$$\fP\(rq
 * (when executed in the top-level macro at least).
 *
 * The \fBquit\fP hook is still executed.
 */
static void
teco_state_control_exit(teco_machine_main_t *ctx, GError **error)
{
	if (teco_undo_enabled) {
		g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
		                    "<^C> not allowed in interactive mode");
		return;
	}

	teco_quit_requested = TRUE;
	g_set_error_literal(error, TECO_ERROR, TECO_ERROR_QUIT, "");
}

/*$ ^O octal
 * ^O -- Set radix to 8 (octal)
 */
static void
teco_state_control_octal(teco_machine_main_t *ctx, GError **error)
{
	teco_set_radix(8);
}

/*$ ^D decimal
 * ^D -- Set radix to 10 (decimal)
 */
static void
teco_state_control_decimal(teco_machine_main_t *ctx, GError **error)
{
	teco_set_radix(10);
}

/*$ ^R radix
 * radix^R -- Set and get radix
 * ^R -> radix
 *
 * Set current radix to arbitrary value <radix>.
 * If <radix> is omitted, the command instead
 * returns the current radix.
 */
static void
teco_state_control_radix(teco_machine_main_t *ctx, GError **error)
{
	if (!teco_expressions_eval(FALSE, error))
		return;
	if (!teco_expressions_args()) {
		teco_expressions_push(teco_radix);
	} else {
		teco_int_t v;
		if (!teco_expressions_pop_num_calc(&v, 0, error))
			return;
		teco_set_radix(v);
	}
}

static teco_state_t *
teco_state_control_input(teco_machine_main_t *ctx, gchar chr, GError **error)
{
	static teco_machine_main_transition_t transitions[] = {
		/*
		 * Simple transitions
		 */
		['I']  = {&teco_state_insert_indent},
		['U']  = {&teco_state_ctlucommand},
		['^']  = {&teco_state_ascii},
		['[']  = {&teco_state_escape},

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
		['C']  = {&teco_state_start, teco_state_control_exit},
		['O']  = {&teco_state_start, teco_state_control_octal},
		['D']  = {&teco_state_start, teco_state_control_decimal},
		['R']  = {&teco_state_start, teco_state_control_radix}
	};

	/*
	 * FIXME: Should we return a special syntax error in case of failure?
	 * Currently you get error messages like 'Syntax error "F"' for ^F.
	 * The easiest way around would be g_prefix_error(error, "Control command");
	 */
	return teco_machine_main_transition_input(ctx, transitions, G_N_ELEMENTS(transitions),
	                                          teco_ascii_toupper(chr), error);
}

TECO_DEFINE_STATE_CASEINSENSITIVE(teco_state_control);

static teco_state_t *
teco_state_ascii_input(teco_machine_main_t *ctx, gchar chr, GError **error)
{
	if (ctx->mode == TECO_MODE_NORMAL)
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
teco_state_escape_input(teco_machine_main_t *ctx, gchar chr, GError **error)
{
	/*$ ^[^[ ^[$ $$ terminate return
	 * [a1,a2,...]$$ -- Terminate command line or return from macro
	 * [a1,a2,...]^[$
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
	 * The first \fIescape\fP of \fB$$\fP may be typed either
	 * as an escape character (ASCII 27), in up-arrow mode
	 * (e.g. \fB^[$\fP) or as a dollar character \(em the
	 * second character must be either a real escape character
	 * or a dollar character.
	 */
	if (chr == '\e' || chr == '$') {
		if (ctx->mode > TECO_MODE_NORMAL)
			return &teco_state_start;

		ctx->parent.current = &teco_state_start;
		if (!teco_expressions_eval(FALSE, error))
			return NULL;
		teco_error_return_set(error, teco_expressions_args());
		return NULL;
	}

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
	if (ctx->mode == TECO_MODE_NORMAL &&
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

TECO_DEFINE_STATE_CASEINSENSITIVE(teco_state_escape,
	.end_of_macro_cb = teco_state_escape_end_of_macro,
	/*
	 * The state should behave like teco_state_start
	 * when it comes to function key macro masking.
	 */
	.is_start = TRUE,
	.fnmacro_mask = TECO_FNMACRO_MASK_START
);

/*$ EF close
 * [bool]EF -- Remove buffer from ring
 * -EF
 *
 * Removes buffer from buffer ring, effectively
 * closing it.
 * If the buffer is dirty (modified), EF will yield
 * an error.
 * <bool> may be a specified to enforce closing dirty
 * buffers.
 * If it is a Failure condition boolean (negative),
 * the buffer will be closed unconditionally.
 * If <bool> is absent, the sign prefix (1 or -1) will
 * be implied, so \(lq-EF\(rq will always close the buffer.
 *
 * It is noteworthy that EF will be executed immediately in
 * interactive mode but can be rubbed out at a later time
 * to reopen the file.
 * Closed files are kept in memory until the command line
 * is terminated.
 */
static void
teco_state_ecommand_close(teco_machine_main_t *ctx, GError **error)
{
	if (teco_qreg_current) {
		const teco_string_t *name = &teco_qreg_current->head.name;
		g_autofree gchar *name_printable = teco_string_echo(name->data, name->len);
		g_set_error(error, TECO_ERROR, TECO_ERROR_FAILED,
		            "Q-Register \"%s\" currently edited", name_printable);
		return;
	}

	teco_int_t v;
	if (!teco_expressions_pop_num_calc(&v, teco_num_sign, error))
		return;
	if (teco_is_failure(v) && teco_ring_current->dirty) {
		g_set_error(error, TECO_ERROR, TECO_ERROR_FAILED,
		            "Buffer \"%s\" is dirty",
			    teco_ring_current->filename ? : "(Unnamed)");
		return;
	}

	teco_ring_close(error);
}

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
 *   - 8: Enable/disable automatic folding of case-insensitive
 *     command characters during interactive key translation.
 *     The case of letter keys is inverted, so one or two
 *     character commands will typically be inserted upper-case,
 *     but you can still press Shift to insert lower-case letters.
 *     Case-insensitive Q-Register specifications are not
 *     case folded.
 *     This is thought to improve the readability of the command
 *     line macro.
 *   - 16: Enable/disable automatic translation of end of
 *     line sequences to and from line feed.
 *     Disabling this flag allows 8-bit clean loading and saving
 *     of files.
 *   - 32: Enable/Disable buffer editing hooks
 *     (via execution of macro in global Q-Register \(lqED\(rq)
 *   - 64: Enable/Disable function key macros
 *   - 128: Enable/Disable enforcement of UNIX98
 *     \(lq/bin/sh\(rq emulation for operating system command
 *     executions
 *   - 256: Enable/Disable \fBxterm\fP(1) clipboard support.
 *     Should only be enabled if XTerm allows the
 *     \fIGetSelection\fP and \fISetSelection\fP window
 *     operations.
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
 * -EJ -> value
 * value,keyEJ
 * rgb,color,3EJ
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
 * .IP 0 4
 * The current user interface: 1 for Curses, 2 for GTK
 * (\fBread-only\fP)
 * .IP 1
 * The current numbfer of buffers: Also the numeric id
 * of the last buffer in the ring. This is implied if
 * no argument is given, so \(lqEJ\(rq returns the number
 * of buffers in the ring.
 * (\fBread-only\fP)
 * .IP 2
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
 * .IP 3
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
 */
static void
teco_state_ecommand_properties(teco_machine_main_t *ctx, GError **error)
{
	enum {
		EJ_USER_INTERFACE = 0,
		EJ_BUFFERS,
		EJ_MEMORY_LIMIT,
		EJ_INIT_COLOR
	};

	teco_int_t property;
	if (!teco_expressions_eval(FALSE, error) ||
	    !teco_expressions_pop_num_calc(&property, teco_num_sign, error))
		return;

	if (teco_expressions_args() > 0) {
		/*
		 * Set property
		 */
		teco_int_t value, color;
		if (!teco_expressions_pop_num_calc(&value, 0, error))
			return;

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
			if (!teco_expressions_pop_num_calc(&color, 0, error))
				return;
			teco_interface_init_color((guint)value, (guint32)color);
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

	default:
		g_set_error(error, TECO_ERROR, TECO_ERROR_FAILED,
		            "Invalid property %" TECO_INT_FORMAT " "
		            "for <EJ>", property);
		return;
	}
}

/*$ EL eol
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

		if (teco_machine_main_eval_colon(ctx)) {
			teco_int_t v1, v2;
			if (!teco_expressions_pop_num_calc(&v1, 0, error))
				return;

			switch (v1) {
			case '\r':
				eol_mode = SC_EOL_CR;
				break;
			case '\n':
				if (!teco_expressions_args()) {
					eol_mode = SC_EOL_LF;
					break;
				}
				if (!teco_expressions_pop_num_calc(&v2, 0, error))
					return;
				if (v2 == '\r') {
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
			if (!teco_expressions_pop_num_calc(&eol_mode, 0, error))
				return;
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
	} else if (teco_machine_main_eval_colon(ctx)) {
		const gchar *eol_seq = teco_eol_get_seq(teco_interface_ssm(SCI_GETEOLMODE, 0, 0));
		teco_expressions_push(eol_seq);
	} else {
		teco_expressions_push(teco_interface_ssm(SCI_GETEOLMODE, 0, 0));
	}
}

/*$ EX exit
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
 */
/** @fixme what if changing file after EX? will currently still exit */
static void
teco_state_ecommand_exit(teco_machine_main_t *ctx, GError **error)
{
	if (teco_machine_main_eval_colon(ctx)) {
		if (!teco_ring_save_all_dirty_buffers(error))
			return;
	} else {
		teco_int_t v;
		if (!teco_expressions_pop_num_calc(&v, teco_num_sign, error))
			return;
		if (teco_is_failure(v) && teco_ring_is_any_dirty()) {
			g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
			                    "Modified buffers exist");
			return;
		}
	}

	teco_undo_gboolean(teco_quit_requested) = TRUE;
}

static teco_state_t *
teco_state_ecommand_input(teco_machine_main_t *ctx, gchar chr, GError **error)
{
	static teco_machine_main_transition_t transitions[] = {
		/*
		 * Simple Transitions
		 */
		['%']  = {&teco_state_epctcommand},
		['B']  = {&teco_state_edit_file},
		['C']  = {&teco_state_execute},
		['G']  = {&teco_state_egcommand},
		['I']  = {&teco_state_insert_nobuilding},
		['M']  = {&teco_state_macrofile},
		['N']  = {&teco_state_glob_pattern},
		['S']  = {&teco_state_scintilla_symbols},
		['Q']  = {&teco_state_eqcommand},
		['U']  = {&teco_state_eucommand},
		['W']  = {&teco_state_save_file},

		/*
		 * Commands
		 */
		['F']  = {&teco_state_start, teco_state_ecommand_close},
		['D']  = {&teco_state_start, teco_state_ecommand_flags},
		['J']  = {&teco_state_start, teco_state_ecommand_properties},
		['L']  = {&teco_state_start, teco_state_ecommand_eol},
		['X']  = {&teco_state_start, teco_state_ecommand_exit}
	};

	/*
	 * FIXME: Should we return a special syntax error in case of failure?
	 */
	return teco_machine_main_transition_input(ctx, transitions, G_N_ELEMENTS(transitions),
	                                          teco_ascii_toupper(chr), error);
}

TECO_DEFINE_STATE_CASEINSENSITIVE(teco_state_ecommand);

gboolean
teco_state_insert_initial(teco_machine_main_t *ctx, GError **error)
{
	if (ctx->mode > TECO_MODE_NORMAL)
		return TRUE;

	if (!teco_expressions_eval(FALSE, error))
		return FALSE;
	guint args = teco_expressions_args();
	if (!args)
		return TRUE;

	teco_interface_ssm(SCI_BEGINUNDOACTION, 0, 0);
	for (int i = args; i > 0; i--) {
		gchar chr = (gchar)teco_expressions_peek_num(i-1);
		teco_interface_ssm(SCI_ADDTEXT, 1, (sptr_t)&chr);
	}
	for (int i = args; i > 0; i--)
		if (!teco_expressions_pop_num_calc(NULL, 0, error))
			return FALSE;
	teco_interface_ssm(SCI_ENDUNDOACTION, 0, 0);
	teco_ring_dirtify();

	if (teco_current_doc_must_undo())
		undo__teco_interface_ssm(SCI_UNDO, 0, 0);

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
 * String building characters are \fBenabled\fP for the
 * I command.
 * When editing \*(ST macros, using the \fBEI\fP command
 * may be better, since it has string building characters
 * disabled.
 */
TECO_DEFINE_STATE_INSERT(teco_state_insert_building);

/*$ EI
 * [c1,c2,...]EI[text]$ -- Insert text without string building characters
 *
 * Inserts text at the current position in the current
 * document.
 * This command is identical to the \fBI\fP command,
 * except that string building characters are \fBdisabled\fP.
 * Therefore it may be beneficial when editing \*(ST
 * macros.
 */
TECO_DEFINE_STATE_INSERT(teco_state_insert_nobuilding,
	.expectstring.string_building = FALSE
);

static gboolean
teco_state_insert_indent_initial(teco_machine_main_t *ctx, GError **error)
{
	if (ctx->mode > TECO_MODE_NORMAL)
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

		gchar spaces[len];

		memset(spaces, ' ', sizeof(spaces));
		teco_interface_ssm(SCI_ADDTEXT, sizeof(spaces), (sptr_t)spaces);
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
