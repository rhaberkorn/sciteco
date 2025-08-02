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
#include "parser.h"
#include "string-utils.h"

/** Check whether c is a non-operational command in teco_state_start */
static inline gboolean
teco_is_noop(gunichar c)
{
	return c == ' ' || c == '\f' || c == '\r' || c == '\n' || c == '\v';
}

gboolean teco_get_range_args(const gchar *cmd, gsize *from_ret, gsize *len_ret, GError **error);

/* in cmdline.c */
gboolean teco_state_command_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx,
                                             gunichar key, GError **error);

/**
 * @class TECO_DEFINE_STATE_COMMAND
 * @implements TECO_DEFINE_STATE_CASEINSENSITIVE
 * @ingroup states
 *
 * Base state for everything where part of a one or two letter command
 * is accepted.
 */
#define TECO_DEFINE_STATE_COMMAND(NAME, ...) \
	TECO_DEFINE_STATE_CASEINSENSITIVE(NAME, \
		.process_edit_cmd_cb = (teco_state_process_edit_cmd_cb_t) \
		                       teco_state_command_process_edit_cmd, \
		.style = SCE_SCITECO_COMMAND, \
		##__VA_ARGS__ \
	)

/*
 * FIXME: Most of these states can probably be private/static
 * as they are only referenced from teco_state_start.
 */
TECO_DECLARE_STATE(teco_state_fcommand);

void teco_undo_change_dir_to_current(void);
TECO_DECLARE_STATE(teco_state_changedir);

TECO_DECLARE_STATE(teco_state_condcommand);
TECO_DECLARE_STATE(teco_state_control);
TECO_DECLARE_STATE(teco_state_ascii);
TECO_DECLARE_STATE(teco_state_ecommand);

typedef struct {
	teco_int_t from;	/*< start position in glyphs */
	teco_int_t to;		/*< end position in glyphs */
} teco_range_t;

extern guint teco_ranges_count;
extern teco_range_t *teco_ranges;

gboolean teco_state_insert_initial(teco_machine_main_t *ctx, GError **error);
gboolean teco_state_insert_process(teco_machine_main_t *ctx, const teco_string_t *str,
                                   gsize new_chars, GError **error);
teco_state_t *teco_state_insert_done(teco_machine_main_t *ctx, const teco_string_t *str, GError **error);

/* in cmdline.c */
gboolean teco_state_insert_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx,
                                            gunichar chr, GError **error);

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

TECO_DECLARE_STATE(teco_state_insert_plain);
TECO_DECLARE_STATE(teco_state_insert_indent);

/**
 * @class TECO_DEFINE_STATE_START
 * @implements TECO_DEFINE_STATE_COMMAND
 * @ingroup states
 *
 * Base state for everything where a new command can begin
 * (the start state itself and all lookahead states).
 */
#define TECO_DEFINE_STATE_START(NAME, ...) \
	TECO_DEFINE_STATE_COMMAND(NAME, \
		.end_of_macro_cb = NULL, /* Allowed at the end of a macro! */ \
		.is_start = TRUE, \
		.keymacro_mask = TECO_KEYMACRO_MASK_START | TECO_KEYMACRO_MASK_CASEINSENSITIVE, \
		##__VA_ARGS__ \
	)

teco_state_t *teco_state_start_input(teco_machine_main_t *ctx, gunichar chr, GError **error);

TECO_DECLARE_STATE(teco_state_start);
TECO_DECLARE_STATE(teco_state_escape);
TECO_DECLARE_STATE(teco_state_ctlc);
TECO_DECLARE_STATE(teco_state_ctlc_control);
