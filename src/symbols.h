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
#pragma once

#include <glib.h>

#include "sciteco.h"
#include "string-utils.h"
#include "parser.h"

typedef struct {
	const gchar *name;
	gint value;
} teco_symbol_entry_t;

typedef struct {
	const teco_symbol_entry_t *entries;
	gint size;

	int (*cmp_fnc)(const char *, const char *, size_t);

	/**
	 * For auto-completions.
	 * The list is allocated only ondemand.
	 */
	GList *list;
} teco_symbol_list_t;

void teco_symbol_list_init(teco_symbol_list_t *ctx, const teco_symbol_entry_t *entries, gint size,
                           gboolean case_sensitive);

gint teco_symbol_list_lookup(teco_symbol_list_t *ctx, const gchar *name, const gchar *prefix);

gboolean teco_symbol_list_auto_complete(teco_symbol_list_t *ctx, const gchar *symbol, teco_string_t *insert);

/** @memberof teco_symbol_list_t */
static inline void
teco_symbol_list_clear(teco_symbol_list_t *ctx)
{
	 g_list_free(ctx->list);
}

extern teco_symbol_list_t teco_symbol_list_scintilla;
extern teco_symbol_list_t teco_symbol_list_scilexer;

/*
 * Command states
 */

TECO_DECLARE_STATE(teco_state_scintilla_symbols);
