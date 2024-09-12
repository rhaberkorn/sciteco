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
#include <glib/gprintf.h>

#include "sciteco.h"
#include "string-utils.h"
#include "undo.h"
#include "rb3str.h"
#include "goto.h"

//#define DEBUG

/** @extends teco_rb3str_head_t */
typedef struct {
	teco_rb3str_head_t head;
	gsize pc;
} teco_goto_label_t;

/** @private @static @memberof teco_goto_label_t */
static teco_goto_label_t *
teco_goto_label_new(const gchar *name, gsize len, gsize pc)
{
	teco_goto_label_t *label = g_new0(teco_goto_label_t, 1);
	teco_string_init(&label->head.name, name, len);
	label->pc = pc;
	return label;
}

/** @private @memberof teco_goto_label_t */
static inline void
teco_goto_label_free(teco_goto_label_t *label)
{
	teco_string_clear(&label->head.name);
	g_free(label);
}

/*
 * FIXME: Most of these methods could be static since
 * they are only called from goto.c.
 */

#ifdef DEBUG
static void
teco_goto_table_dump(teco_goto_table_t *ctx)
{
	for (rb3_head *cur = rb3_get_min(&ctx->tree);
	     cur != NULL;
	     cur = rb3_get_next(cur)) {
		teco_goto_label_t *label = (teco_goto_label_t *)cur;
		g_autofree *label_printable;
		label_printable = teco_string_echo(cur->head.key.data, cur->head.key.len);

		g_printf("table[\"%s\"] = %d\n", label_printable, label->pc);
	}
	g_printf("---END---\n");
}
#endif

/** @memberof teco_goto_table_t */
gssize
teco_goto_table_remove(teco_goto_table_t *ctx, const gchar *name, gsize len)
{
	gssize existing_pc = -1;

	teco_goto_label_t *label = (teco_goto_label_t *)teco_rb3str_find(&ctx->tree, TRUE, name, len);
	if (label) {
		existing_pc = label->pc;
		rb3_unlink_and_rebalance(&label->head.head);
		teco_goto_label_free(label);
	}

	return existing_pc;
}

/** @memberof teco_goto_table_t */
gssize
teco_goto_table_find(teco_goto_table_t *ctx, const gchar *name, gsize len)
{
	teco_goto_label_t *label = (teco_goto_label_t *)teco_rb3str_find(&ctx->tree, TRUE, name, len);
	return label ? label->pc : -1;
}

/** @memberof teco_goto_table_t */
gssize
teco_goto_table_set(teco_goto_table_t *ctx, const gchar *name, gsize len, gssize pc)
{
	if (pc < 0)
		return teco_goto_table_remove(ctx, name, len);

	gssize existing_pc = -1;

	teco_goto_label_t *label = (teco_goto_label_t *)teco_rb3str_find(&ctx->tree, TRUE, name, len);
	if (label) {
		existing_pc = label->pc;
		label->pc = pc;
	} else {
		label = teco_goto_label_new(name, len, pc);
		teco_rb3str_insert(&ctx->tree, TRUE, &label->head);
	}

#ifdef DEBUG
	teco_goto_table_dump(ctx);
#endif

	return existing_pc;
}

/*
 * NOTE: We don't simply TECO_DEFINE_UNDO_CALL(), so we can store `name`
 * as part of the undo token.
 * If it would be a temporary pointer, TECO_DEFINE_UNDO_CALL() wouldn't
 * do anyway.
 */
typedef struct {
	teco_goto_table_t *table;
	gssize pc;
	gsize len;
	gchar name[];
} teco_goto_table_undo_set_t;

static void
teco_goto_table_undo_set_action(teco_goto_table_undo_set_t *ctx, gboolean run)
{
	if (run) {
		teco_goto_table_set(ctx->table, ctx->name, ctx->len, ctx->pc);
#ifdef DEBUG
		teco_goto_table_dump(ctx->table);
#endif
	}
}

/** @memberof teco_goto_table_t */
void
teco_goto_table_undo_set(teco_goto_table_t *ctx, const gchar *name, gsize len, gssize pc)
{
	if (!ctx->must_undo)
		return;

	teco_goto_table_undo_set_t *token;
	token = teco_undo_push_size((teco_undo_action_t)teco_goto_table_undo_set_action,
	                            sizeof(*token) + len);
	if (token) {
		token->table = ctx;
		token->pc = pc;
		token->len = len;
		if (name)
			memcpy(token->name, name, len);
	}
}

/** @memberof teco_goto_table_t */
void
teco_goto_table_clear(teco_goto_table_t *ctx)
{
	struct rb3_head *cur;

	while ((cur = rb3_get_root(&ctx->tree))) {
		rb3_unlink_and_rebalance(cur);
		teco_goto_label_free((teco_goto_label_t *)cur);
	}
}
