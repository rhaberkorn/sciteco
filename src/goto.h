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
#pragma once

#include <glib.h>

#include "sciteco.h"
#include "string-utils.h"
#include "rb3str.h"

/** @extends teco_rb3str_tree_t */
typedef struct {
	teco_rb3str_tree_t tree;

	/**
	 * Whether to generate undo tokens (unnecessary in macro invocations)
	 */
	gboolean must_undo;
} teco_goto_table_t;

/** @memberof teco_goto_table_t */
static inline void
teco_goto_table_init(teco_goto_table_t *ctx, gboolean must_undo)
{
	rb3_reset_tree(&ctx->tree);
	ctx->must_undo = must_undo;
}

gint teco_goto_table_remove(teco_goto_table_t *ctx, const gchar *name, gsize len);

gint teco_goto_table_find(teco_goto_table_t *ctx, const gchar *name, gsize len);

gint teco_goto_table_set(teco_goto_table_t *ctx, const gchar *name, gsize len, gint pc);
void teco_goto_table_undo_set(teco_goto_table_t *ctx, const gchar *name, gsize len, gint pc);

/** @memberof teco_goto_table_t */
static inline gboolean
teco_goto_table_auto_complete(teco_goto_table_t *ctx, const gchar *str, gsize len,
                              teco_string_t *insert)
{
	return teco_rb3str_auto_complete(&ctx->tree, TRUE, str, len, 0, insert);
}

void teco_goto_table_clear(teco_goto_table_t *ctx);
