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

#include <glib.h>

#include "sciteco.h"
#include "string-utils.h"
#include "expressions.h"
#include "parser.h"
#include "lexer.h"
#include "core-commands.h"
#include "undo.h"
#include "goto.h"
#include "goto-commands.h"

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
 *
 * TODO: Add support for "true" comments of the form !* ... *!
 * This would be almost trivial to implement, but if we don't
 * want any (even temporary) overhead for comments at all, we need
 * to add a new parser state.
 * I'm unsure whether !-signs should be allowed within comments.
 */
static teco_state_t *
teco_state_label_input(teco_machine_main_t *ctx, gunichar chr, GError **error)
{
	if (chr == '!') {
		/*
		 * NOTE: If the label already existed, its PC will be restored
		 * on rubout.
		 * Otherwise, the label will be removed (PC == -1).
		 */
		gssize existing_pc = teco_goto_table_set(&ctx->goto_table, ctx->goto_label.data,
		                                         ctx->goto_label.len, ctx->macro_pc);
		if (ctx->parent.must_undo)
			teco_goto_table_undo_set(&ctx->goto_table, ctx->goto_label.data, ctx->goto_label.len, existing_pc);

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
gboolean teco_state_goto_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx, gunichar chr, GError **error);

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
	.process_edit_cmd_cb = (teco_state_process_edit_cmd_cb_t)teco_state_goto_process_edit_cmd
);
