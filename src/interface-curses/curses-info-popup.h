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

#include <curses.h>

#include "list.h"
#include "interface.h"

typedef struct {
	WINDOW *window;			/**! window showing part of pad */
	WINDOW *pad;			/**! full-height entry list */

	teco_stailq_head_t list;	/**! list of popup entries */
	gint longest;			/**! size of longest entry */
	gint length;			/**! total number of popup entries */

	gint pad_first_line;		/**! first line in pad to show */

	GStringChunk *chunk;		/**! string chunk for all popup entry names */
} teco_curses_info_popup_t;

static inline void
teco_curses_info_popup_init(teco_curses_info_popup_t *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->list = TECO_STAILQ_HEAD_INITIALIZER(&ctx->list);
}

void teco_curses_info_popup_add(teco_curses_info_popup_t *ctx, teco_popup_entry_type_t type,
                                const gchar *name, gsize name_len, gboolean highlight);

void teco_curses_info_popup_show(teco_curses_info_popup_t *ctx, attr_t attr);
static inline bool
teco_curses_info_popup_is_shown(teco_curses_info_popup_t *ctx)
{
	return ctx->window != NULL;
}

static inline void
teco_curses_info_popup_noutrefresh(teco_curses_info_popup_t *ctx)
{
	if (ctx->window)
		wnoutrefresh(ctx->window);
}

void teco_curses_info_popup_clear(teco_curses_info_popup_t *ctx);
