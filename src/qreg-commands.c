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

#include <Scintilla.h>

#include "sciteco.h"
#include "error.h"
#include "file-utils.h"
#include "expressions.h"
#include "interface.h"
#include "ring.h"
#include "parser.h"
#include "core-commands.h"
#include "qreg.h"
#include "qreg-commands.h"

gboolean
teco_state_expectqreg_initial(teco_machine_main_t *ctx, GError **error)
{
	teco_state_t *current = ctx->parent.current;

	/*
	 * NOTE: This could theoretically be allocated once in
	 * teco_machine_main_init(), but we'd have to set the type here anyway.
	 */
	ctx->expectqreg = teco_machine_qregspec_new(current->expectqreg.type, ctx->qreg_table_locals,
	                                            ctx->parent.must_undo);
	if (ctx->parent.must_undo)
		undo__teco_machine_qregspec_clear(&ctx->expectqreg);
	return TRUE;
}

teco_state_t *
teco_state_expectqreg_input(teco_machine_main_t *ctx, gunichar chr, GError **error)
{
	teco_state_t *current = ctx->parent.current;

	teco_qreg_t *qreg = NULL;
	teco_qreg_table_t *table = NULL;

	switch (teco_machine_qregspec_input(ctx->expectqreg, chr,
	                                    ctx->flags.mode == TECO_MODE_NORMAL ? &qreg : NULL, &table, error)) {
	case TECO_MACHINE_QREGSPEC_ERROR:
		return NULL;
	case TECO_MACHINE_QREGSPEC_MORE:
		return current;
	case TECO_MACHINE_QREGSPEC_DONE:
		break;
	}

	/*
	 * NOTE: ctx->expectqreg is preserved since we may want to query it from follow-up
	 * states. This means, it must usually be reset manually in got_register_cb() via:
	 * teco_state_expectqreg_reset(ctx);
	 */
	return current->expectqreg.got_register_cb(ctx, qreg, table, error);
}

static teco_state_t *
teco_state_pushqreg_got_register(teco_machine_main_t *ctx, teco_qreg_t *qreg,
                                 teco_qreg_table_t *table, GError **error)
{
	teco_state_expectqreg_reset(ctx);

	return ctx->flags.mode == TECO_MODE_NORMAL &&
	       !teco_qreg_stack_push(qreg, error) ? NULL : &teco_state_start;
}

/*$ "[" "[q" push
 * [q -- Save Q-Register
 *
 * Save Q-Register <q> contents on the global Q-Register push-down
 * stack.
 */
TECO_DEFINE_STATE_EXPECTQREG(teco_state_pushqreg);

static teco_state_t *
teco_state_popqreg_got_register(teco_machine_main_t *ctx, teco_qreg_t *qreg,
                                teco_qreg_table_t *table, GError **error)
{
	teco_state_expectqreg_reset(ctx);

	if (ctx->flags.mode > TECO_MODE_NORMAL)
		return &teco_state_start;

	if (!teco_machine_main_eval_colon(ctx))
		return !teco_qreg_stack_pop(qreg, error) ? NULL : &teco_state_start;
	teco_expressions_push(teco_bool(teco_qreg_stack_pop(qreg, NULL)));
	return &teco_state_start;
}

/*$ "]" "]q" ":]q" pop
 * ]q -- Restore Q-Register
 * :]q -> Success|Failure
 *
 * Restore Q-Register <q> by replacing its contents
 * with the contents of the register saved on top of
 * the Q-Register push-down stack.
 * The stack entry is popped.
 *
 * When colon-modified, \fB]\fP returns a success boolean
 * (-1) if there was a register to pop.
 * If the stack was empty, a failure boolean (0) is returned
 * instead of throwing an error.
 *
 * In interactive mode, the original contents of <q>
 * are not immediately reclaimed but are kept in memory
 * to support rubbing out the command.
 * Memory is reclaimed on command-line termination.
 */
TECO_DEFINE_STATE_EXPECTQREG(teco_state_popqreg,
	.expectqreg.type = TECO_QREG_OPTIONAL_INIT
);

static teco_state_t *
teco_state_eqcommand_got_register(teco_machine_main_t *ctx, teco_qreg_t *qreg,
                                  teco_qreg_table_t *table, GError **error)
{
	/*
	 * NOTE: We will query ctx->expectqreg later in teco_state_loadqreg_done().
	 */
	return &teco_state_loadqreg;
}

TECO_DEFINE_STATE_EXPECTQREG(teco_state_eqcommand,
	.expectqreg.type = TECO_QREG_OPTIONAL_INIT
);

static teco_state_t *
teco_state_loadqreg_done(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	teco_qreg_t *qreg;
	teco_qreg_table_t *table;

	teco_machine_qregspec_get_results(ctx->expectqreg, &qreg, &table);
	teco_state_expectqreg_reset(ctx);

	if (ctx->flags.mode > TECO_MODE_NORMAL)
		return &teco_state_start;

	if (str->len > 0) {
		/* Load file into Q-Register */
		g_autofree gchar *filename = teco_file_expand_path(str->data);
		if (!qreg->vtable->load(qreg, filename, error))
			return NULL;
	} else {
		/* Edit Q-Register */
		if (!teco_current_doc_undo_edit(error) ||
		    !teco_qreg_table_edit(table, qreg, error))
			return NULL;
	}

	return &teco_state_start;
}

/*$ EQ EQq
 * EQq$ -- Edit or load Q-Register
 * EQq[file]$
 *
 * When specified with an empty <file> string argument,
 * EQ makes <q> the currently edited Q-Register.
 * Otherwise, when <file> is specified, it is the
 * name of a file to read into Q-Register <q>.
 * When loading a file, the currently edited
 * buffer/register is not changed and the edit position
 * of register <q> is reset to 0.
 *
 * Undefined Q-Registers will be defined.
 * The command fails if <file> could not be read.
 */
TECO_DEFINE_STATE_EXPECTFILE(teco_state_loadqreg);

static teco_state_t *
teco_state_epctcommand_got_register(teco_machine_main_t *ctx, teco_qreg_t *qreg,
                                    teco_qreg_table_t *table, GError **error)
{
	/*
	 * NOTE: We will query ctx->expectqreg later in teco_state_saveqreg_done().
	 */
	return &teco_state_saveqreg;
}

TECO_DEFINE_STATE_EXPECTQREG(teco_state_epctcommand);

static teco_state_t *
teco_state_saveqreg_done(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	teco_qreg_t *qreg;

	teco_machine_qregspec_get_results(ctx->expectqreg, &qreg, NULL);
	teco_state_expectqreg_reset(ctx);

	if (ctx->flags.mode > TECO_MODE_NORMAL)
		return &teco_state_start;

	g_autofree gchar *filename = teco_file_expand_path(str->data);
	return qreg->vtable->save(qreg, filename, error) ? &teco_state_start : NULL;
}

/*$ E% E%q
 * E%q<file>$ -- Save Q-Register string to file
 *
 * Saves the string contents of Q-Register <q> to
 * <file>.
 * The <file> must always be specified, as Q-Registers
 * have no notion of associated file names.
 *
 * In interactive mode, the E% command may be rubbed out,
 * restoring the previous state of <file>.
 * This follows the same rules as with the \fBEW\fP command.
 *
 * File names may also be tab-completed and string building
 * characters are enabled by default.
 */
TECO_DEFINE_STATE_EXPECTFILE(teco_state_saveqreg);

static gboolean
teco_state_queryqreg_initial(teco_machine_main_t *ctx, GError **error)
{
	/*
	 * This prevents teco_state_queryqreg_got_register() from having to check
	 * for Q-Register existence, resulting in better error messages in case of
	 * required Q-Registers.
	 * In parse-only mode, the type does not matter.
	 */
	teco_qreg_type_t type = ctx->flags.modifier_colon ? TECO_QREG_OPTIONAL : TECO_QREG_REQUIRED;

	/*
	 * NOTE: We have to allocate a new instance always since `expectqreg`
	 * is part of an union.
	 */
	ctx->expectqreg = teco_machine_qregspec_new(type, ctx->qreg_table_locals,
	                                            ctx->parent.must_undo);
	if (ctx->parent.must_undo)
		undo__teco_machine_qregspec_clear(&ctx->expectqreg);
	return TRUE;
}

static teco_state_t *
teco_state_queryqreg_got_register(teco_machine_main_t *ctx, teco_qreg_t *qreg,
                                  teco_qreg_table_t *table, GError **error)
{
	teco_state_expectqreg_reset(ctx);

	if (ctx->flags.mode > TECO_MODE_NORMAL)
		return &teco_state_start;

	if (!teco_expressions_eval(FALSE, error))
		return NULL;

	if (teco_machine_main_eval_colon(ctx) > 0) {
		/* Query Q-Register's existence or string size */
		if (qreg) {
			/* get_string() would return the size in bytes */
			teco_int_t len = qreg->vtable->get_length(qreg, error);
			if (len < 0)
				return NULL;
			teco_expressions_push(len);
		} else {
			teco_expressions_push(-1);
		}

		return &teco_state_start;
	}

	if (teco_expressions_args() > 0) {
		/* Query character from Q-Register string */
		teco_int_t pos = teco_expressions_pop_num(0);
		if (pos < 0) {
			teco_error_range_set(error, "Q");
			return NULL;
		}

		teco_int_t c;
		if (!qreg->vtable->get_character(qreg, pos, &c, error))
			return NULL;
		teco_expressions_push(c);
	} else {
		/* Query integer */
		teco_int_t value;

		if (!qreg->vtable->get_integer(qreg, &value, error))
			return NULL;
		teco_expressions_push(value);
	}

	return &teco_state_start;
}

/*$ "Q" "Qq" ":Qq" query
 * Qq -> n -- Query Q-Register existence, its integer or string characters
 * -Qq -> -n
 * <position>Qq -> code
 * :Qq -> -1 | size
 *
 * Without any arguments, get and return the integer-part of
 * Q-Register <q>.
 *
 * With one argument, return the character <code> at <position>
 * from the string-part of Q-Register <q>.
 * Positions are handled like buffer positions \(em they
 * begin at 0 up to the length of the string minus 1.
 * -1 is returned for invalid positions.
 * If <q> is encoded as UTF-8 and there is
 * an invalid byte sequence at the requested position,
 * -2 is returned.
 * Incomplete UTF-8 byte sequences are returned as -3.
 * Both non-colon-modified forms of Q require register <q>
 * to be defined and fail otherwise.
 *
 * When colon-modified, Q does not pop any arguments from
 * the expression stack and returns the <size> of the string
 * in Q-Register <q> if register <q> exists (i.e. is defined).
 * Naturally, for empty strings, 0 is returned.
 * When colon-modified and Q-Register <q> is undefined,
 * -1 is returned instead.
 * Therefore checking the return value \fB:Q\fP for values smaller
 * 0 allows checking the existence of a register.
 * Note that if <q> exists, its string part is not initialized,
 * so \fB:Q\fP may be used to handle purely numeric data structures
 * without creating Scintilla documents by accident.
 * These semantics allow the useful idiom \(lq:Q\fIq\fP">\(rq for
 * checking whether a Q-Register exists and has a non-empty string.
 * Note also that the return value of \fB:Q\fP may be interpreted
 * as a condition boolean that represents the non-existence of <q>.
 * If <q> is undefined, it returns \fIsuccess\fP, else a \fIfailure\fP
 * boolean.
 */
TECO_DEFINE_STATE_EXPECTQREG(teco_state_queryqreg,
	.initial_cb = (teco_state_initial_cb_t)teco_state_queryqreg_initial
);

static teco_state_t *
teco_state_ctlucommand_got_register(teco_machine_main_t *ctx, teco_qreg_t *qreg,
                                    teco_qreg_table_t *table, GError **error)
{
	/*
	 * NOTE: We will query ctx->expectqreg later in teco_state_setqregstring_nobuilding_done().
	 */
	return &teco_state_setqregstring_nobuilding;
}

TECO_DEFINE_STATE_EXPECTQREG(teco_state_ctlucommand,
	.expectqreg.type = TECO_QREG_OPTIONAL_INIT
);

static teco_state_t *
teco_state_setqregstring_nobuilding_done(teco_machine_main_t *ctx,
                                         const teco_string_t *str, GError **error)
{
	teco_qreg_t *qreg;

	teco_machine_qregspec_get_results(ctx->expectqreg, &qreg, NULL);
	teco_state_expectqreg_reset(ctx);

	if (ctx->flags.mode > TECO_MODE_NORMAL)
		return &teco_state_start;

	gboolean colon_modified = teco_machine_main_eval_colon(ctx) > 0;

	if (!teco_expressions_eval(FALSE, error))
		return NULL;
	gint args = teco_expressions_args();

	if (args > 0) {
		guint codepage = teco_default_codepage();
		if (colon_modified && !qreg->vtable->get_string(qreg, NULL, NULL, &codepage, error))
			return NULL;

		g_autofree gchar *buffer = NULL;
		const gchar *start;
		gsize len = 0;

		if (codepage == SC_CP_UTF8) {
			/* 4 bytes should be enough for UTF-8, but we better follow the documentation */
			start = buffer = g_malloc(args*6);

			for (gint i = args; i > 0; i--) {
				teco_int_t chr = teco_expressions_peek_num(i-1);
				if (chr < 0 || !g_unichar_validate(chr)) {
					teco_error_codepoint_set(error, "^U");
					return NULL;
				}
				len += g_unichar_to_utf8(chr, buffer+len);
			}
			/* we pop only now since we had to peek in reverse order */
			for (gint i = 0; i < args; i++)
				teco_expressions_pop_num(0);
		} else {
			buffer = g_malloc(args);

			for (gint i = 0; i < args; i++) {
				teco_int_t chr = teco_expressions_pop_num(0);
				if (chr < 0 || chr > 0xFF) {
					teco_error_codepoint_set(error, "^U");
					return NULL;
				}
				buffer[args-(++len)] = chr;
			}
			start = buffer+args-len;
		}

		if (colon_modified) {
			/* append to register */
			if (!qreg->vtable->append_string(qreg, start, len, error))
				return NULL;
		} else {
			/* set register */
			if (!qreg->vtable->undo_set_string(qreg, error) ||
			    !qreg->vtable->set_string(qreg, start, len,
			                              codepage, error))
				return NULL;
		}
	}

	if (args > 0 || colon_modified) {
		/* append to register */
		if (!qreg->vtable->append_string(qreg, str->data, str->len, error))
			return NULL;
	} else {
		/* set register */
		if (!qreg->vtable->undo_set_string(qreg, error) ||
		    !qreg->vtable->set_string(qreg, str->data, str->len,
		                              teco_default_codepage(), error))
			return NULL;
	}

	return &teco_state_start;
}

/*$ "^Uq" ":^Uq" "set string" append
 * [c1,c2,...]^Uq[string]$ -- Set or append to Q-Register string without string building
 * [c1,c2,...]:^Uq[string]$
 *
 * If not colon-modified, it first fills the Q-Register <q>
 * with all the values on the expression stack (interpreted as
 * codepoints).
 * It does so in the order of the arguments, i.e.
 * <c1> will be the first character in <q>, <c2> the second, etc.
 * Eventually the <string> argument is appended to the
 * register.
 * Any existing string value in <q> is overwritten by this operation.
 *
 * In the colon-modified form ^U does not overwrite existing
 * contents of <q> but only appends to it.
 *
 * If <q> is undefined, it will be defined.
 *
 * String-building characters are \fBdisabled\fP for ^U
 * commands.
 * Therefore they are especially well-suited for defining
 * \*(ST macros, since string building characters in the
 * desired Q-Register contents do not have to be escaped.
 * The \fBEU\fP command may be used where string building
 * is desired.
 */
TECO_DEFINE_STATE_EXPECTSTRING(teco_state_setqregstring_nobuilding,
	.expectstring.string_building = FALSE
);

static teco_state_t *
teco_state_eucommand_got_register(teco_machine_main_t *ctx, teco_qreg_t *qreg,
                                  teco_qreg_table_t *table, GError **error)
{
	/*
	 * NOTE: We will query ctx->expectqreg later in teco_state_setqregstring_building_done().
	 */
	return &teco_state_setqregstring_building;
}

TECO_DEFINE_STATE_EXPECTQREG(teco_state_eucommand,
	.expectqreg.type = TECO_QREG_OPTIONAL_INIT
);

static gboolean
teco_state_setqregstring_building_initial(teco_machine_main_t *ctx, GError **error)
{
	if (ctx->flags.mode > TECO_MODE_NORMAL)
		return TRUE;

	teco_qreg_t *qreg;
	teco_machine_qregspec_get_results(ctx->expectqreg, &qreg, NULL);

	/*
	 * The expected codepage of string building constructs is determined
	 * by the Q-Register.
	 */
	guint codepage;
	if (!qreg->vtable->get_string(qreg, NULL, NULL, &codepage, error))
		return FALSE;
	teco_machine_stringbuilding_set_codepage(&ctx->expectstring.machine, codepage);
	return TRUE;
}

static teco_state_t *
teco_state_setqregstring_building_done(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	return teco_state_setqregstring_nobuilding_done(ctx, str, error);
}

/*$ "EU" "EUq" ":EUq"
 * [c1,c2,...]EUq[string]$ -- Set or append to Q-Register string with string building characters
 * [c1,c2,...]:EUq[string]$
 *
 * This command sets or appends to the contents of
 * Q-Register \fIq\fP.
 * It is identical to the \fB^U\fP command, except
 * that this form of the command has string building
 * characters \fBenabled\fP.
 */
TECO_DEFINE_STATE_EXPECTSTRING(teco_state_setqregstring_building,
	.initial_cb = (teco_state_initial_cb_t)teco_state_setqregstring_building_initial,
	.expectstring.string_building = TRUE
);

static teco_state_t *
teco_state_getqregstring_got_register(teco_machine_main_t *ctx, teco_qreg_t *qreg,
                                      teco_qreg_table_t *table, GError **error)
{
	teco_state_expectqreg_reset(ctx);

	if (ctx->flags.mode > TECO_MODE_NORMAL)
		return &teco_state_start;

	g_auto(teco_string_t) str = {NULL, 0};

	if (!qreg->vtable->get_string(qreg, &str.data, &str.len, NULL, error))
		return NULL;

	if (teco_machine_main_eval_colon(ctx)) {
		teco_interface_msg_literal(TECO_MSG_USER, str.data, str.len);
		return &teco_state_start;
	}

	sptr_t pos = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);

	if (str.len > 0) {
		teco_interface_ssm(SCI_BEGINUNDOACTION, 0, 0);
		teco_interface_ssm(SCI_ADDTEXT, str.len, (sptr_t)str.data);
		teco_interface_ssm(SCI_ENDUNDOACTION, 0, 0);
		teco_ring_dirtify();

		if (teco_current_doc_must_undo())
			undo__teco_interface_ssm(SCI_UNDO, 0, 0);
	}

	teco_undo_int(teco_ranges[0].from) = teco_interface_bytes2glyphs(pos);
	teco_undo_int(teco_ranges[0].to) = teco_interface_bytes2glyphs(pos + str.len);
	teco_undo_guint(teco_ranges_count) = 1;

	return &teco_state_start;
}

/*$ G Gq get paste
 * Gq -- Insert or print Q-Register string
 * :Gq
 *
 * Inserts the string of Q-Register <q> into the buffer
 * at its current position.
 * If colon-modified prints the string as a message
 * (i.e. to the terminal and/or in the message area) instead
 * of modifying the current buffer.
 *
 * Specifying an undefined <q> yields an error.
 */
TECO_DEFINE_STATE_EXPECTQREG(teco_state_getqregstring);

static teco_state_t *
teco_state_setqreginteger_got_register(teco_machine_main_t *ctx, teco_qreg_t *qreg,
                                       teco_qreg_table_t *table, GError **error)
{
	teco_state_expectqreg_reset(ctx);

	if (ctx->flags.mode > TECO_MODE_NORMAL)
		return &teco_state_start;

	if (!teco_expressions_eval(FALSE, error))
		return NULL;
	if (teco_expressions_args() || teco_num_sign < 0) {
		teco_int_t v;
		if (!teco_expressions_pop_num_calc(&v, teco_num_sign, error) ||
		    !qreg->vtable->undo_set_integer(qreg, error) ||
		    !qreg->vtable->set_integer(qreg, v, error))
			return NULL;

		if (teco_machine_main_eval_colon(ctx) > 0)
			teco_expressions_push(TECO_SUCCESS);
	} else if (teco_machine_main_eval_colon(ctx) > 0) {
		teco_expressions_push(TECO_FAILURE);
	} else {
		teco_error_argexpected_set(error, "U");
		return NULL;
	}

	return &teco_state_start;
}

/*$ "U" "Uq" ":Uq" set
 * nUq -- Set Q-Register integer
 * -Uq
 * [n]:Uq -> Success|Failure
 *
 * Sets the integer-part of Q-Register <q> to <n>.
 * \(lq-U\(rq is equivalent to \(lq-1U\(rq, otherwise
 * the command fails if <n> is missing.
 *
 * If the command is colon-modified, it returns a success
 * boolean if <n> or \(lq-\(rq is given.
 * Otherwise it returns a failure boolean and does not
 * modify <q>.
 *
 * The register is defined if it does not exist.
 */
TECO_DEFINE_STATE_EXPECTQREG(teco_state_setqreginteger,
	.expectqreg.type = TECO_QREG_OPTIONAL_INIT
);

static teco_state_t *
teco_state_increaseqreg_got_register(teco_machine_main_t *ctx, teco_qreg_t *qreg,
                                     teco_qreg_table_t *table, GError **error)
{
	teco_state_expectqreg_reset(ctx);

	if (ctx->flags.mode > TECO_MODE_NORMAL)
		return &teco_state_start;

	teco_int_t value, add;

	if (!qreg->vtable->undo_set_integer(qreg, error) ||
	    !qreg->vtable->get_integer(qreg, &value, error) ||
	    !teco_expressions_pop_num_calc(&add, teco_num_sign, error) ||
	    !qreg->vtable->set_integer(qreg, value += add, error))
		return NULL;
	teco_expressions_push(value);

	return &teco_state_start;
}

/*$ % %q increment decrement
 * [n]%q -> q+n -- Increase or decrease Q-Register integer
 * -%q -> q-1
 *
 * Add <n> to the integer part of register <q>, returning
 * its new value.
 * If <n> is omitted, the sign prefix is implied.
 * <q> will be defined if it does not exist.
 */
TECO_DEFINE_STATE_EXPECTQREG(teco_state_increaseqreg,
	.expectqreg.type = TECO_QREG_OPTIONAL_INIT
);

static teco_state_t *
teco_state_macro_got_register(teco_machine_main_t *ctx, teco_qreg_t *qreg,
                              teco_qreg_table_t *table, GError **error)
{
	teco_state_expectqreg_reset(ctx);

	if (ctx->flags.mode > TECO_MODE_NORMAL)
		return &teco_state_start;

	if (teco_machine_main_eval_colon(ctx) > 0) {
		/* don't create new local Q-Registers if colon modifier is given */
		if (!teco_qreg_execute(qreg, ctx->qreg_table_locals, error))
			return NULL;
	} else {
		g_auto(teco_qreg_table_t) table;
		teco_qreg_table_init_locals(&table, FALSE);

		if (!teco_qreg_execute(qreg, &table, error))
			return NULL;
		if (teco_qreg_table_current == &table) {
			/* currently editing local Q-Register that's about to be freed */
			teco_error_editinglocalqreg_set(error, teco_qreg_current->head.name.data,
			                                teco_qreg_current->head.name.len);
			return NULL;
		}
	}

	return &teco_state_start;
}

/*$ "M" "Mq" ":Mq" call eval macro
 * Mq -- Execute macro
 * :Mq
 *
 * Execute macro stored in string of Q-Register <q>.
 * The command itself does not push or pop and arguments from the stack
 * but the macro executed might well do so.
 * The new macro invocation level will contain its own go-to label table
 * and local Q-Register table.
 * Except when the command is colon-modified - in this case, local
 * Q-Registers referenced in the macro refer to the parent macro-level's
 * local Q-Register table (or whatever level defined one last).
 *
 * Errors during the macro execution will propagate to the M command.
 * In other words if a command in the macro fails, the M command will fail
 * and this failure propagates until the top-level macro (e.g.
 * the command-line macro).
 *
 * Note that the string of <q> will be copied upon macro execution,
 * so subsequent changes to Q-Register <q> from inside the macro do
 * not modify the executed code.
 *
 * While \fBM\fP does not check the register's configured encoding
 * (as reported by \fBEE\fP), its contents must be and are checked to be in
 * valid UTF-8.
 */
TECO_DEFINE_STATE_EXPECTQREG(teco_state_macro);

static teco_state_t *
teco_state_indirect_done(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	if (ctx->flags.mode > TECO_MODE_NORMAL)
		return &teco_state_start;

	g_autofree gchar *filename = teco_file_expand_path(str->data);

	if (teco_machine_main_eval_colon(ctx) > 0) {
		/* don't create new local Q-Registers if colon modifier is given */
		if (!teco_execute_file(filename, ctx->qreg_table_locals, error))
			return NULL;
	} else {
		g_auto(teco_qreg_table_t) table;
		teco_qreg_table_init_locals(&table, FALSE);

		if (!teco_execute_file(filename, &table, error))
			return NULL;
	}

	return &teco_state_start;
}

/*$ "EI" ":EI" indirect include
 * EIfile$ -- Execute from indirect command file
 * :EIfile$
 *
 * Read the file with name <file> into memory and execute its contents
 * as a macro.
 * It is otherwise similar to the \(lqM\(rq command.
 *
 * If <file> could not be read, the command yields an error.
 *
 * As all \*(ST code, the contents of <file> must be in valid UTF-8
 * even if operating in the \(lqdefault ANSI\(rq mode as configured by \fBED\fP.
 */
TECO_DEFINE_STATE_EXPECTFILE(teco_state_indirect);

static teco_state_t *
teco_state_copytoqreg_got_register(teco_machine_main_t *ctx, teco_qreg_t *qreg,
                                   teco_qreg_table_t *table, GError **error)
{
	teco_state_expectqreg_reset(ctx);

	/*
	 * NOTE: "@" has syntactic significance in most contexts, so it's set
	 * in parse-only mode.
	 * Therefore, it must also be evaluated in parse-only mode, even though
	 * it has no syntactic significance for Xq.
	 */
	gboolean modifier_at = teco_machine_main_eval_at(ctx);

	if (ctx->flags.mode > TECO_MODE_NORMAL)
		return &teco_state_start;

	gsize from, len;

	if (!teco_get_range_args("X", &from, &len, error))
		return NULL;

	/*
	 * NOTE: This does not use SCI_GETRANGEPOINTER+SCI_GETGAPPOSITION
	 * since it may not be safe when copying from register to register.
	 */
	g_autofree gchar *str = g_malloc(len + 1);

	struct Sci_TextRangeFull range = {
		.chrg = {from, from + len},
		.lpstrText = str
	};
	teco_interface_ssm(SCI_GETTEXTRANGEFULL, 0, (sptr_t)&range);

	if (teco_machine_main_eval_colon(ctx) > 0) {
		if (!qreg->vtable->append_string(qreg, str, len, error))
			return NULL;
	} else {
		guint cp = teco_interface_get_codepage();

		if (!qreg->vtable->undo_set_string(qreg, error) ||
		    !qreg->vtable->set_string(qreg, str, len, cp, error))
			return NULL;
	}

	if (!modifier_at || len == 0)
		return &teco_state_start;

	/*
	 * If @-modified, cut into the register
	 */
	if (teco_current_doc_must_undo()) {
		sptr_t pos = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);
		undo__teco_interface_ssm(SCI_GOTOPOS, pos, 0);
		undo__teco_interface_ssm(SCI_UNDO, 0, 0);
	}

	/*
	 * Should always generate an undo action.
	 */
	teco_interface_ssm(SCI_BEGINUNDOACTION, 0, 0);
	teco_interface_ssm(SCI_DELETERANGE, from, len);
	teco_interface_ssm(SCI_ENDUNDOACTION, 0, 0);
	teco_ring_dirtify();

	return &teco_state_start;
}

/*$ "X" "Xq" ":Xq" "@Xq" ":@Xq" copy extract
 * [lines]Xq -- Copy into or append or cut to Q-Register
 * -Xq
 * from,toXq
 * [lines]:Xq
 * -:Xq
 * from,to:Xq
 * [lines]@Xq
 * -@Xq
 * from,to@Xq
 * [lines]:@Xq
 * -:@Xq
 * from,to:@Xq
 *
 * Copy the next or previous number of <lines> from the buffer
 * into the Q-Register <q> string.
 * If <lines> is omitted, the sign prefix is implied.
 * If two arguments are specified, the characters beginning
 * at position <from> up to the character at position <to>
 * are copied.
 * The semantics of the arguments is analogous to the \fBK\fP
 * command's arguments.
 *
 * If the command is colon-modified (\fB:\fP), the characters will be
 * appended to the end of register <q> instead.
 * If the command is at-modified (\fB@\fP), the text will be
 * removed from the buffer after copying or appending to the
 * Q-Register, thus performing a cut operation.
 * The order of modifiers is as always insignificant.
 *
 * Register <q> will be created if it is undefined.
 */
TECO_DEFINE_STATE_EXPECTQREG(teco_state_copytoqreg,
	.expectqreg.type = TECO_QREG_OPTIONAL_INIT
);
