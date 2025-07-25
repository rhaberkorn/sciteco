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

#include <glib.h>

#include "sciteco.h"
#include "parser.h"
#include "error.h"
#include "undo.h"
#include "expressions.h"
#include "interface.h"
#include "cmdline.h"
#include "core-commands.h"
#include "stdio-commands.h"

/**
 * Print number from stack in the given radix.
 *
 * It must be popped manually, so we can call it multiple times
 * on the same number.
 */
static gboolean
teco_print(teco_machine_main_t *ctx, guint radix, GError **error)
{
	if (!teco_expressions_eval(FALSE, error))
		return FALSE;
	if (!teco_expressions_args()) {
		teco_error_argexpected_set(error, "=");
		return FALSE;
	}

	/*
	 * teco_expressions_format() cannot easily be used
	 * to format __unsigned__ integers.
	 */
	const gchar *fmt = "%" TECO_INT_MODIFIER "d";
	switch (radix) {
	case 8:  fmt = "%" TECO_INT_MODIFIER "o"; break;
	case 16: fmt = "%" TECO_INT_MODIFIER "X"; break;
	}
	gchar buf[32];
	gint len = g_snprintf(buf, sizeof(buf), fmt, teco_expressions_peek_num(0));
	g_assert(len > 0);
	if (!teco_machine_main_eval_colon(ctx))
		buf[len++] = '\n';

	teco_interface_msg_literal(TECO_MSG_USER, buf, len);
	return TRUE;
}

/*$ "=" "==" "===" ":=" ":==" ":===" "print number"
 * <n>= -- Print integer as message
 * <n>==
 * <n>===
 * <n>:=
 * <n>:==
 * <n>:===
 *
 * Shows integer <n> as a message in the message line and/or
 * on the console.
 * One \(lq=\(rq formats the integer as a signed decimal number,
 * \(lq==\(rq formats as an unsigned octal number and
 * \(lq===\(rq as an unsigned hexadecimal number.
 * It is logged with the user-message severity.
 * The command fails if <n> is not given.
 *
 * A noteworthy quirk is that \(lq==\(rq and \(lq===\(rq
 * will print 2 or 3 numbers in succession when executed
 * from interactive mode at the end of the command line
 * in order to guarantee immediate feedback.
 *
 * If you want to print multiple values from the stack,
 * you have to put the \(lq=\(rq into a pass-through loop
 * or separate the commands with
 * whitespace (e.g. \(lq^Y= =\(rq).
 *
 * If colon-modified the number is printed without a trailing
 * linefeed.
 */
/*
 * In order to imitate TECO-11 closely, we apply the lookahead
 * strategy -- `=` and `==` are not executed immediately but only
 * when a non-`=` character is parsed (cf. `$$` and `^C^C`).
 * However, this would be very annoying during interactive
 * execution, therefore we still print the number immediately
 * and perhaps multiple times:
 * Typing `===` prints the number first in decimal,
 * then octal and finally in hexadecimal.
 * This won't happen e.g. in a loop that is closed on the command-line.
 */
TECO_DECLARE_STATE(teco_state_print_octal);

static gboolean
teco_state_print_decimal_initial(teco_machine_main_t *ctx, GError **error)
{
	if (ctx->flags.mode > TECO_MODE_NORMAL || !teco_cmdline_is_executing(ctx))
		return TRUE;
	/*
	 * Interactive invocation:
	 * don't yet pop number as we may have to print it repeatedly
	 */
	return teco_print(ctx, 10, error);
}

static teco_state_t *
teco_state_print_decimal_input(teco_machine_main_t *ctx, gunichar chr, GError **error)
{
	if (chr == '=')
		return &teco_state_print_octal;

	if (ctx->flags.mode == TECO_MODE_NORMAL) {
		if (!teco_cmdline_is_executing(ctx) && !teco_print(ctx, 10, error))
			return NULL;
		teco_expressions_pop_num(0);
	}
	return teco_state_start_input(ctx, chr, error);
}

/*
 * Due to the deferred nature of `=`,
 * it is valid to end in this state as well.
 */
static gboolean
teco_state_print_decimal_end_of_macro(teco_machine_main_t *ctx, GError **error)
{
	if (teco_cmdline_is_executing(ctx) || ctx->flags.mode > TECO_MODE_NORMAL)
		return TRUE;
	if (!teco_print(ctx, 10, error))
		return FALSE;
	teco_expressions_pop_num(0);
	return TRUE;
}

TECO_DEFINE_STATE_START(teco_state_print_decimal,
	.initial_cb = (teco_state_initial_cb_t)teco_state_print_decimal_initial,
	.end_of_macro_cb = (teco_state_end_of_macro_cb_t)
	                   teco_state_print_decimal_end_of_macro
);

static gboolean
teco_state_print_octal_initial(teco_machine_main_t *ctx, GError **error)
{
	if (ctx->flags.mode > TECO_MODE_NORMAL || !teco_cmdline_is_executing(ctx))
		return TRUE;
	/*
	 * Interactive invocation:
	 * don't yet pop number as we may have to print it repeatedly
	 */
	return teco_print(ctx, 8, error);
}

static teco_state_t *
teco_state_print_octal_input(teco_machine_main_t *ctx, gunichar chr, GError **error)
{
	if (chr == '=') {
		if (ctx->flags.mode == TECO_MODE_NORMAL) {
			if (!teco_print(ctx, 16, error))
				return NULL;
			teco_expressions_pop_num(0);
		}
		return &teco_state_start;
	}

	if (ctx->flags.mode == TECO_MODE_NORMAL) {
		if (!teco_cmdline_is_executing(ctx) && !teco_print(ctx, 8, error))
			return NULL;
		teco_expressions_pop_num(0);
	}
	return teco_state_start_input(ctx, chr, error);
}

/*
 * Due to the deferred nature of `==`,
 * it is valid to end in this state as well.
 */
static gboolean
teco_state_print_octal_end_of_macro(teco_machine_main_t *ctx, GError **error)
{
	if (teco_cmdline_is_executing(ctx) || ctx->flags.mode > TECO_MODE_NORMAL)
		return TRUE;
	if (!teco_print(ctx, 8, error))
		return FALSE;
	teco_expressions_pop_num(0);
	return TRUE;
}

TECO_DEFINE_STATE_START(teco_state_print_octal,
	.initial_cb = (teco_state_initial_cb_t)teco_state_print_octal_initial,
	.end_of_macro_cb = (teco_state_end_of_macro_cb_t)
	                   teco_state_print_octal_end_of_macro
);

static gboolean
teco_state_print_string_initial(teco_machine_main_t *ctx, GError **error)
{
	/*
	 * ^A differs from all other string-taking commands in having
	 * a default ^A escape char.
	 */
	if (ctx->parent.must_undo)
		teco_undo_gunichar(ctx->expectstring.machine.escape_char);
	ctx->expectstring.machine.escape_char = TECO_CTL_KEY('A');

	/* chain to the default initial_cb */
	return teco_state_expectstring_initial(ctx, error);
}

static teco_state_t *
teco_state_print_string_done(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	teco_interface_msg_literal(TECO_MSG_USER, str->data, str->len);
	return &teco_state_start;
}

/*$ "^A" print "print string"
 * ^A<string>^A -- Print string as message
 * @^A/string/
 *
 * Print <string> as a message, i.e. in the message line
 * in interactive mode and if possible on the terminal (stdout) as well.
 *
 * \fB^A\fP differs from all other commands in the way <string>
 * is terminated.
 * It is terminated by ^A (CTRL+A, ASCII 1) by default.
 * While the initial \fB^A\fP can be written with upcarets,
 * the terminating ^A must always be ASCII 1.
 * You can however overwrite the <string> terminator as usual
 * by \fB@\fP-modifying the command.
 *
 * String-building characters are enabled for this command.
 */
TECO_DEFINE_STATE_EXPECTSTRING(teco_state_print_string,
	.initial_cb = (teco_state_initial_cb_t)teco_state_print_string_initial
);
