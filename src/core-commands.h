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
#include "parser.h"
#include "string-utils.h"

/*
 * FIXME: Most of these states can probably be private/static
 * as they are only referenced from teco_state_start.
 */
TECO_DECLARE_STATE(teco_state_start);
TECO_DECLARE_STATE(teco_state_fcommand);

void teco_undo_change_dir_to_current(void);
TECO_DECLARE_STATE(teco_state_changedir);

TECO_DECLARE_STATE(teco_state_condcommand);
TECO_DECLARE_STATE(teco_state_control);
TECO_DECLARE_STATE(teco_state_ascii);
TECO_DECLARE_STATE(teco_state_escape);
TECO_DECLARE_STATE(teco_state_ecommand);

typedef struct {
	gsize from;	/*< start position in bytes */
	gsize to;	/*< end position in bytes */
} teco_range_t;

extern guint teco_ranges_count;
extern teco_range_t *teco_ranges;

gboolean teco_state_insert_initial(teco_machine_main_t *ctx, GError **error);
gboolean teco_state_insert_process(teco_machine_main_t *ctx, const teco_string_t *str,
                                   gsize new_chars, GError **error);
teco_state_t *teco_state_insert_done(teco_machine_main_t *ctx, const teco_string_t *str, GError **error);

/* in cmdline.c */
gboolean teco_state_insert_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx, gunichar chr, GError **error);

/**
 * @class TECO_DEFINE_STATE_INSERT
 * @implements TECO_DEFINE_STATE_EXPECTSTRING
 * @ingroup states
 *
 * @note Also serves as a base class of the replace-insertion commands.
 * @fixme Generating the done_cb could be avoided if there simply were a default.
 */
#define TECO_DEFINE_STATE_INSERT(NAME, ...) \
	static teco_state_t * \
	NAME##_done(teco_machine_main_t *ctx, const teco_string_t *str, GError **error) \
	{ \
		return teco_state_insert_done(ctx, str, error); \
	} \
	TECO_DEFINE_STATE_EXPECTSTRING(NAME, \
		.initial_cb = (teco_state_initial_cb_t)teco_state_insert_initial, \
		.process_edit_cmd_cb = (teco_state_process_edit_cmd_cb_t)teco_state_insert_process_edit_cmd, \
		.expectstring.process_cb = teco_state_insert_process, \
		##__VA_ARGS__ \
	)

TECO_DECLARE_STATE(teco_state_insert_building);
TECO_DECLARE_STATE(teco_state_insert_nobuilding);
TECO_DECLARE_STATE(teco_state_insert_indent);
