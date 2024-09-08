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

#include <string.h>

#include <glib.h>

#include <Scintilla.h>

#include "sciteco.h"
#include "view.h"
#include "undo.h"

/**
 * Scintilla document type.
 * The struct is never defined and only exists for improved
 * type safety.
 */
typedef struct teco_doc_scintilla_t teco_doc_scintilla_t;

/**
 * A Scintilla document.
 *
 * Also contains other attributes required to restore
 * the overall editor state when loading it into a Scintilla view.
 */
typedef struct {
	/**
	 * Underlying Scintilla document.
	 * It is created on demand in teco_doc_get_scintilla(),
	 * so that we don't waste memory on integer-only Q-Registers.
	 */
	teco_doc_scintilla_t *doc;

	/*
	 * The so called "parameters".
	 * Updated/restored only when required
	 */
	gint anchor, dot;
	gint first_line, xoffset;
} teco_doc_t;

/** @memberof teco_doc_t */
static inline void
teco_doc_init(teco_doc_t *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
}

void teco_doc_edit(teco_doc_t *ctx);
void teco_doc_undo_edit(teco_doc_t *ctx);

void teco_doc_set_string(teco_doc_t *ctx, const gchar *str, gsize len, guint codepage);
void teco_doc_undo_set_string(teco_doc_t *ctx);

void teco_doc_get_string(teco_doc_t *ctx, gchar **str, gsize *len, guint *codepage);

void teco_doc_update_from_view(teco_doc_t *ctx, teco_view_t *from);
void teco_doc_update_from_doc(teco_doc_t *ctx, const teco_doc_t *from);

/** @memberof teco_doc_t */
#define teco_doc_update(CTX, FROM) \
	(_Generic((FROM), teco_view_t *      : teco_doc_update_from_view, \
	                  teco_doc_t *       : teco_doc_update_from_doc, \
	                  const teco_doc_t * : teco_doc_update_from_doc)((CTX), (FROM)))

/** @memberof teco_doc_t */
static inline void
teco_doc_reset(teco_doc_t *ctx)
{
	ctx->anchor = ctx->dot = 0;
	ctx->first_line = ctx->xoffset = 0;
}

/** @memberof teco_doc_t */
static inline void
teco_doc_undo_reset(teco_doc_t *ctx)
{
	/*
	 * NOTE: Could be rolled into one function
	 * and called with teco_undo_call() if we really
	 * wanted to save more memory.
	 */
	teco_undo_gint(ctx->anchor);
	teco_undo_gint(ctx->dot);
	teco_undo_gint(ctx->first_line);
	teco_undo_gint(ctx->xoffset);
}

void teco_doc_exchange(teco_doc_t *ctx, teco_doc_t *other);

/** @memberof teco_doc_t */
static inline void
teco_doc_undo_exchange(teco_doc_t *ctx)
{
	teco_undo_ptr(ctx->doc);
	teco_doc_undo_reset(ctx);
}

void teco_doc_clear(teco_doc_t *ctx);
