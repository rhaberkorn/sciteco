/*
 * Copyright (C) 2012-2022 Robin Haberkorn
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
#include "undo.h"
#include "qreg.h"
#include "view.h"
#include "parser.h"
#include "list.h"

typedef struct teco_buffer_t {
	teco_tailq_entry_t entry;

	teco_view_t *view;

	gchar *filename;
	gboolean dirty;
} teco_buffer_t;

/** @memberof teco_buffer_t */
static inline teco_buffer_t *
teco_buffer_next(teco_buffer_t *ctx)
{
	return (teco_buffer_t *)ctx->entry.next;
}

/** @memberof teco_buffer_t */
static inline teco_buffer_t *
teco_buffer_prev(teco_buffer_t *ctx)
{
	return (teco_buffer_t *)ctx->entry.prev->prev->next;
}

void teco_buffer_edit(teco_buffer_t *ctx);
void teco_buffer_undo_edit(teco_buffer_t *ctx);

extern teco_buffer_t *teco_ring_current;

teco_buffer_t *teco_ring_first(void);
teco_buffer_t *teco_ring_last(void);

teco_int_t teco_ring_get_id(teco_buffer_t *buffer);

teco_buffer_t *teco_ring_find_by_name(const gchar *filename);
teco_buffer_t *teco_ring_find_by_id(teco_int_t id);

#define teco_ring_find(X) \
	(_Generic((X), gchar *       : teco_ring_find_by_name, \
	               const gchar * : teco_ring_find_by_name, \
	               teco_int_t    : teco_ring_find_by_id)(X))

void teco_ring_dirtify(void);
gboolean teco_ring_is_any_dirty(void);
gboolean teco_ring_save_all_dirty_buffers(GError **error);

gboolean teco_ring_edit_by_name(const gchar *filename, GError **error);
gboolean teco_ring_edit_by_id(teco_int_t id, GError **error);

#define teco_ring_edit(X, ERROR) \
	(_Generic((X), gchar *       : teco_ring_edit_by_name, \
	               const gchar * : teco_ring_edit_by_name, \
	               teco_int_t    : teco_ring_edit_by_id)((X), (ERROR)))

static inline void
teco_ring_undo_edit(void)
{
	teco_undo_ptr(teco_qreg_current);
	teco_undo_ptr(teco_ring_current);
	teco_buffer_undo_edit(teco_ring_current);
}

gboolean teco_ring_close(GError **error);
void teco_ring_undo_close(void);

void teco_ring_set_scintilla_undo(gboolean state);

void teco_ring_cleanup(void);

/*
 * Command states
 */

TECO_DECLARE_STATE(teco_state_edit_file);
TECO_DECLARE_STATE(teco_state_save_file);

/*
 * Helper functions applying to any current
 * document (whether a buffer or QRegister).
 * There's currently no better place to put them.
 */
static inline gboolean
teco_current_doc_undo_edit(GError **error)
{
	if (!teco_qreg_current) {
		teco_ring_undo_edit();
		return TRUE;
	}

	teco_undo_ptr(teco_qreg_current);
	return teco_qreg_current->vtable->undo_edit(teco_qreg_current, error);
}

static inline gboolean
teco_current_doc_must_undo(void)
{
	/*
	 * If there's no currently edited Q-Register
	 * we must be editing the current buffer
	 */
	return !teco_qreg_current || teco_qreg_current->must_undo;
}
