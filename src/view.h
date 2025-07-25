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

#include <Scintilla.h>

#include "sciteco.h"

/**
 * @interface teco_view_t
 * Interface for all SciTECO views.
 *
 * Methods that must still be implemented in the user-interface
 * layer are marked with the \@pure tag.
 */
typedef struct teco_view_t teco_view_t;

/** @pure @static @memberof teco_view_t */
teco_view_t *teco_view_new(void);

void teco_view_setup(teco_view_t *ctx);

/** @pure @memberof teco_view_t */
sptr_t teco_view_ssm(teco_view_t *ctx, unsigned int iMessage, uptr_t wParam, sptr_t lParam);

/** @memberof teco_view_t */
void undo__teco_view_ssm(teco_view_t *, unsigned int, uptr_t, sptr_t);

void teco_view_set_representations(teco_view_t *ctx);

/** @memberof teco_view_t */
static inline void
teco_view_set_scintilla_undo(teco_view_t *ctx, gboolean state)
{
	teco_view_ssm(ctx, SCI_EMPTYUNDOBUFFER, 0, 0);
	teco_view_ssm(ctx, SCI_SETUNDOCOLLECTION, state, 0);
}

gboolean teco_view_load_from_channel(teco_view_t *ctx, GIOChannel *channel,
                                     gboolean clear, GError **error);
gboolean teco_view_load_from_file(teco_view_t *ctx, const gchar *filename,
                                  gboolean clear, GError **error);

/** @memberof teco_view_t */
#define teco_view_load(CTX, FROM, CLEAR, ERROR) \
	(_Generic((FROM), GIOChannel *  : teco_view_load_from_channel, \
	                  gchar *       : teco_view_load_from_file, \
	                  const gchar * : teco_view_load_from_file)((CTX), (FROM), \
	                                                            (CLEAR), (ERROR)))

gboolean teco_view_save_to_channel(teco_view_t *ctx, GIOChannel *channel, GError **error);
gboolean teco_view_save_to_file(teco_view_t *ctx, const gchar *filename, GError **error);

/** @memberof teco_view_t */
#define teco_view_save(CTX, TO, ERROR) \
	(_Generic((TO), GIOChannel *  : teco_view_save_to_channel, \
	                gchar *       : teco_view_save_to_file, \
	                const gchar * : teco_view_save_to_file)((CTX), (TO), (ERROR)))

/** @pure @memberof teco_view_t */
void teco_view_free(teco_view_t *ctx);

static inline guint
teco_view_get_codepage(teco_view_t *ctx)
{
	return teco_view_ssm(ctx, SCI_GETCODEPAGE, 0, 0)
		? : teco_view_ssm(ctx, SCI_STYLEGETCHARACTERSET, STYLE_DEFAULT, 0);
}

gssize teco_view_glyphs2bytes(teco_view_t *ctx, teco_int_t pos);
teco_int_t teco_view_bytes2glyphs(teco_view_t *ctx, gsize pos);
gssize teco_view_glyphs2bytes_relative(teco_view_t *ctx, gsize pos, teco_int_t n);

teco_int_t teco_view_get_character(teco_view_t *ctx, gsize pos, gsize len);

void teco_view_process_notify(teco_view_t *ctx, SCNotification *notify);
