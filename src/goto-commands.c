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

teco_string_t teco_goto_skip_label = {NULL, 0};

static gboolean
teco_state_label_initial(teco_machine_main_t *ctx, GError **error)
{
	memset(&ctx->goto_label, 0, sizeof(ctx->goto_label));
	return TRUE;
}

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
		if (existing_pc == ctx->macro_pc)
			/* encountered the same label again */
			return &teco_state_start;
		if (existing_pc >= 0) {
			g_autofree gchar *label_printable = teco_string_echo(ctx->goto_label.data,
			                                                     ctx->goto_label.len);
			teco_interface_msg(TECO_MSG_WARNING, "Ignoring goto label \"%s\" redefinition",
			                   label_printable);
			return &teco_state_start;
		}
		if (ctx->parent.must_undo)
			teco_goto_table_undo_remove(&ctx->goto_table, ctx->goto_label.data, ctx->goto_label.len);

		if (teco_goto_skip_label.len > 0 &&
		    !teco_string_cmp(&ctx->goto_label, teco_goto_skip_label.data, teco_goto_skip_label.len)) {
			teco_undo_string_own(teco_goto_skip_label);
			memset(&teco_goto_skip_label, 0, sizeof(teco_goto_skip_label));

			if (ctx->parent.must_undo)
				teco_undo_guint(ctx->__flags);
			ctx->mode = TECO_MODE_NORMAL;
		}

		if (ctx->parent.must_undo)
			teco_undo_string_own(ctx->goto_label);
		else
			teco_string_clear(&ctx->goto_label);
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
	.initial_cb = (teco_state_initial_cb_t)teco_state_label_initial,
	.style = SCE_SCITECO_LABEL
);

static teco_state_t *
teco_state_goto_done(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	if (ctx->mode > TECO_MODE_NORMAL)
		return &teco_state_start;

	teco_int_t value;
	if (!teco_expressions_pop_num_calc(&value, 1, error))
		return NULL;

	/*
	 * Find the comma-separated substring in str indexed by `value`.
	 */
	teco_string_t label = {NULL, 0};
	while (value > 0) {
		label.data = label.data ? label.data+label.len+1 : str->data;
		const gchar *p = label.data ? memchr(label.data, ',', str->len - (label.data - str->data)) : NULL;
		label.len = p ? p - label.data : str->len - (label.data - str->data);

		value--;

		if (!p)
			break;
	}

	if (value == 0) {
		gssize pc = teco_goto_table_find(&ctx->goto_table, label.data, label.len);

		if (pc >= 0) {
			ctx->macro_pc = pc;
		} else {
			/* skip till label is defined */
			g_assert(teco_goto_skip_label.len == 0);
			undo__teco_string_truncate(&teco_goto_skip_label, 0);
			teco_string_init(&teco_goto_skip_label, label.data, label.len);
			if (ctx->parent.must_undo)
				teco_undo_guint(ctx->__flags);
			ctx->mode = TECO_MODE_PARSE_ONLY_GOTO;
		}
	}

	return &teco_state_start;
}

/* in cmdline.c */
gboolean teco_state_goto_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx,
                                          gunichar chr, GError **error);
gboolean teco_state_goto_insert_completion(teco_machine_main_t *ctx, const teco_string_t *str,
                                           GError **error);

/*$ O
 * Olabel$ -- Go to label
 * [n]Olabel1[,label2,...]$
 *
 * Go to <label>.
 * The simple go-to command is a special case of the
 * computed go-to command.
 * A comma-separated list of labels may be specified
 * in the string argument.
 * The label to jump to is selected by <n> (1 is <label1>,
 * 2 is <label2>, etc.).
 * If <n> is omitted, 1 is implied.
 *
 * If the label selected by <n> is does not exist in the
 * list of labels, the command does nothing.
 * Label definitions are cached in a table, so that
 * if the label to go to has already been defined, the
 * go-to command will jump immediately.
 * Otherwise, parsing continues until the <label>
 * is defined.
 * The command will yield an error if a label has
 * not been defined when the macro or command-line
 * is terminated.
 * In the latter case, the user will not be able to
 * terminate the command-line.
 */
TECO_DEFINE_STATE_EXPECTSTRING(teco_state_goto,
	.process_edit_cmd_cb = (teco_state_process_edit_cmd_cb_t)teco_state_goto_process_edit_cmd,
	.insert_completion_cb = (teco_state_insert_completion_cb_t)teco_state_goto_insert_completion
);

/*
 * True comments:
 * They don't add entries to the goto table.
 *
 * NOTE: This still needs some special handling in the Scintilla lexer
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

static teco_state_t *
teco_state_eolcomment_input(teco_machine_main_t *ctx, gunichar chr, GError **error)
{
	return chr == '\n' ? &teco_state_start : &teco_state_eolcomment;
}

TECO_DEFINE_STATE_COMMENT(teco_state_eolcomment);
