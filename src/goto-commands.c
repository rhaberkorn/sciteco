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

#include <glib.h>

#include "sciteco.h"
#include "error.h"
#include "string-utils.h"
#include "expressions.h"
#include "parser.h"
#include "lexer.h"
#include "core-commands.h"
#include "undo.h"
#include "interface.h"
#include "goto.h"
#include "goto-commands.h"

TECO_DECLARE_STATE(teco_state_blockcomment);
TECO_DECLARE_STATE(teco_state_eolcomment);

/**
 * In TECO_MODE_PARSE_ONLY_GOTO mode, we remain in parse-only mode
 * until the given label is encountered.
 */
teco_string_t teco_goto_skip_label = {NULL, 0};
/**
 * The program counter to restore if the teco_goto_skip_label
 * is \b not found (after :Olabel$).
 * If smaller than 0 an error is thrown instead.
 */
gssize teco_goto_backup_pc = -1;

/*
 * NOTE: The comma is theoretically not allowed in a label
 * (see <O> syntax), but is accepted anyway since labels
 * are historically used as comments.
 * SciTECO has true block and EOL comments, though as well.
 */
static teco_state_t *
teco_state_label_input(teco_machine_main_t *ctx, gunichar chr, GError **error)
{
	if (!ctx->goto_label.len) {
		switch (chr) {
		case '*': return &teco_state_blockcomment;	/* `!*` */
		case '!': return &teco_state_eolcomment;	/* `!!` */
		}
	}

	if (chr == '!') {
		gssize existing_pc = teco_goto_table_set(&ctx->goto_table, ctx->goto_label.data,
		                                         ctx->goto_label.len, ctx->macro_pc);
		if (existing_pc < 0) {
			/* new label */
			if (ctx->parent.must_undo)
				teco_goto_table_undo_remove(&ctx->goto_table, ctx->goto_label.data, ctx->goto_label.len);

			if (teco_goto_skip_label.len > 0 &&
			    !teco_string_cmp(&ctx->goto_label, teco_goto_skip_label.data, teco_goto_skip_label.len)) {
				teco_undo_string_own(teco_goto_skip_label);
				memset(&teco_goto_skip_label, 0, sizeof(teco_goto_skip_label));
				teco_undo_gssize(teco_goto_backup_pc) = -1;

				if (ctx->parent.must_undo)
					teco_undo_flags(ctx->flags);
				ctx->flags.mode = TECO_MODE_NORMAL;
			}
		} else if (existing_pc != ctx->macro_pc) {
			g_autofree gchar *label_printable = teco_string_echo(ctx->goto_label.data,
			                                                     ctx->goto_label.len);
			teco_interface_msg(TECO_MSG_WARNING, "Ignoring goto label \"%s\" redefinition",
			                   label_printable);
		}

		if (ctx->parent.must_undo)
			teco_undo_string_own(ctx->goto_label);
		else
			teco_string_clear(&ctx->goto_label);
		memset(&ctx->goto_label, 0, sizeof(ctx->goto_label));

		return &teco_state_start;
	}

	/*
	 * The goto label is collected in parse-only mode as well
	 * since we could jump into a currently dead branch later.
	 *
	 * FIXME: Theoretically, we could avoid that at least in TECO_MODE_LEXING.
	 */
	if (ctx->parent.must_undo)
		undo__teco_string_truncate(&ctx->goto_label, ctx->goto_label.len);
	teco_string_append_wc(&ctx->goto_label, chr);
	return &teco_state_label;
}

TECO_DEFINE_STATE(teco_state_label,
	.style = SCE_SCITECO_LABEL
);

static teco_state_t *
teco_state_goto_done(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	if (ctx->flags.mode > TECO_MODE_NORMAL)
		return &teco_state_start;

	if (!str->len) {
		/* you can still write @O/,/, though... */
		g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
		                    "No labels given for <O>");
		return NULL;
	}

	teco_int_t value;
	if (!teco_expressions_pop_num_calc(&value, 0, error))
		return NULL;

	gboolean colon_modified = teco_machine_main_eval_colon(ctx) > 0;

	/*
	 * Find the comma-separated substring in str indexed by `value`.
	 */
	teco_string_t label = {NULL, 0};
	while (value >= 0) {
		label.data = label.data ? label.data+label.len+1 : str->data;
		const gchar *p = label.data ? memchr(label.data, ',', str->len - (label.data - str->data)) : NULL;
		label.len = p ? p - label.data : str->len - (label.data - str->data);

		value--;

		if (!p)
			break;
	}

	if (value < 0 && label.len > 0) {
		gssize pc = teco_goto_table_find(&ctx->goto_table, label.data, label.len);

		if (pc >= 0) {
			ctx->macro_pc = pc;
		} else if (!ctx->goto_table.complete) {
			/* skip till label is defined */
			g_assert(teco_goto_skip_label.len == 0);
			undo__teco_string_truncate(&teco_goto_skip_label, 0);
			teco_string_init(&teco_goto_skip_label, label.data, label.len);
			teco_undo_gssize(teco_goto_backup_pc) = colon_modified ? ctx->macro_pc : -1;
			if (ctx->parent.must_undo)
				teco_undo_flags(ctx->flags);
			ctx->flags.mode = TECO_MODE_PARSE_ONLY_GOTO;
		} else if (!colon_modified) {
			/* can happen if we previously executed a colon-modified go-to */
			teco_error_label_set(error, teco_goto_skip_label.data, teco_goto_skip_label.len);
			return NULL;
		}
	}

	return &teco_state_start;
}

/* in cmdline.c */
gboolean teco_state_goto_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx,
                                          gunichar chr, GError **error);
gboolean teco_state_goto_insert_completion(teco_machine_main_t *ctx, const teco_string_t *str,
                                           GError **error);

/*$ "O" goto
 * Olabel$ -- Go to label
 * :Olabel$
 * [n]Olabel0[,label1,...]$
 *
 * Go to <label>.
 * The simple go-to command is a special case of the
 * computed go-to command.
 * A comma-separated list of labels may be specified
 * in the string argument.
 * The label to jump to is selected by <n> (0 is <label0>,
 * 1 is <label1>, etc.).
 * If <n> is omitted, 0 is implied.
 * Computed go-tos can be used like switch-case statements
 * other languages.
 *
 * If the label selected by <n> is does not exist in the
 * list of labels or is empty, the command does nothing
 * and execution continues normally.
 * Label definitions are cached in a table, so that
 * if the label to go to has already been defined, the
 * go-to command will jump immediately.
 * Otherwise, parsing continues until the <label>
 * is defined.
 * The command will yield an error if a label has
 * not been defined when the macro is terminated.
 * When jumping to a non-existent <label> in the
 * command-line macro, you cannot practically terminate
 * the command-line until defining the <label>.
 *
 * String building constructs are enabled in \fBO\fP
 * which allows for a second kind of computed go-to,
 * where the label name contains the value to select.
 * When colon-modifying the \fBO\fP command, execution
 * will continue after the command if the given <label>
 * isn't found.
 * This is useful to handle the \(lqdefault\(rq case
 * when using computed go-tos of the second kind.
 */
TECO_DEFINE_STATE_EXPECTSTRING(teco_state_goto,
	.process_edit_cmd_cb = (teco_state_process_edit_cmd_cb_t)teco_state_goto_process_edit_cmd,
	.insert_completion_cb = (teco_state_insert_completion_cb_t)teco_state_goto_insert_completion
);

/**
 * @interface TECO_DEFINE_STATE_COMMENT
 * @implements TECO_DEFINE_STATE
 * @ingroup states
 *
 * True comments:
 * They don't add entries to the goto table.
 *
 * @note This still needs some special handling in the Scintilla lexer
 * (for syntax highlighting) since comments always start with `!`.
 */
#define TECO_DEFINE_STATE_COMMENT(NAME, ...) \
	TECO_DEFINE_STATE(NAME, \
		.style = SCE_SCITECO_COMMENT, \
		##__VA_ARGS__ \
	)

static teco_state_t *
teco_state_blockcomment_star_input(teco_machine_main_t *ctx, gunichar chr, GError **error)
{
	return chr == '!' ? &teco_state_start : &teco_state_blockcomment;
}

TECO_DEFINE_STATE_COMMENT(teco_state_blockcomment_star);

static teco_state_t *
teco_state_blockcomment_input(teco_machine_main_t *ctx, gunichar chr, GError **error)
{
	return chr == '*' ? &teco_state_blockcomment_star : &teco_state_blockcomment;
}

TECO_DEFINE_STATE_COMMENT(teco_state_blockcomment);

/*
 * `!!` line comments are inspired by TECO-64.
 */
static teco_state_t *
teco_state_eolcomment_input(teco_machine_main_t *ctx, gunichar chr, GError **error)
{
	return chr == '\n' ? &teco_state_start : &teco_state_eolcomment;
}

TECO_DEFINE_STATE_COMMENT(teco_state_eolcomment);
