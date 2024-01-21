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

#include <rb3ptr.h>

#include "sciteco.h"
#include "string-utils.h"

/**
 * A RB-tree with teco_string_t keys.
 *
 * NOTE: If the tree's keys do not change and you will never have to free
 * an individual node, consider storing the keys in GStringChunk or
 * allocating them via teco_string_init_chunk(), as this is faster
 * and more memory efficient.
 *
 * FIXME: Perhaps we should simply directly import and tweak rb3tree.c
 * instead of trying to wrap it.
 */
typedef struct rb3_tree teco_rb3str_tree_t;

/** @extends rb3_head */
typedef struct {
	struct rb3_head head;
	/**
	 * The union exists only to allow a "name" alias for "key".
	 */
	union {
		teco_string_t name;
		teco_string_t key;
	};
} teco_rb3str_head_t;

/** @memberof teco_rb3str_head_t */
static inline teco_rb3str_head_t *
teco_rb3str_get_next(teco_rb3str_head_t *head)
{
	return (teco_rb3str_head_t *)rb3_get_next(&head->head);
}

teco_rb3str_head_t *teco_rb3str_insert(teco_rb3str_tree_t *tree, gboolean case_sensitive,
                                       teco_rb3str_head_t *head);

teco_rb3str_head_t *teco_rb3str_find(teco_rb3str_tree_t *tree, gboolean case_sensitive,
                                     const gchar *str, gsize len);

teco_rb3str_head_t *teco_rb3str_nfind(teco_rb3str_tree_t *tree, gboolean case_sensitive,
                                      const gchar *str, gsize len);

gboolean teco_rb3str_auto_complete(teco_rb3str_tree_t *tree, gboolean case_sensitive,
                                   const gchar *str, gsize str_len, gsize restrict_len,
                                   teco_string_t *insert);
