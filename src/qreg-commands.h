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
#include "lexer.h"
#include "qreg.h"

static inline void
teco_state_expectqreg_reset(teco_machine_main_t *ctx)
{
	if (ctx->parent.must_undo)
		teco_undo_qregspec_own(ctx->expectqreg);
	else
		teco_machine_qregspec_free(ctx->expectqreg);
	ctx->expectqreg = NULL;
}

gboolean teco_state_expectqreg_initial(teco_machine_main_t *ctx, GError **error);

teco_state_t *teco_state_expectqreg_input(teco_machine_main_t *ctx, gunichar chr, GError **error);

/* in cmdline.c */
gboolean teco_state_expectqreg_process_edit_cmd(teco_machine_main_t *ctx, teco_machine_t *parent_ctx,
                                                gunichar key, GError **error);
gboolean teco_state_expectqreg_insert_completion(teco_machine_main_t *ctx, const teco_string_t *str,
                                                 GError **error);

/**
 * @interface TECO_DEFINE_STATE_EXPECTQREG
 * @implements TECO_DEFINE_STATE
 * @ingroup states
 *
 * Super class for states accepting Q-Register specifications.
 */
#define TECO_DEFINE_STATE_EXPECTQREG(NAME, ...) \
	static teco_state_t * \
	NAME##_input(teco_machine_main_t *ctx, gunichar chr, GError **error) \
	{ \
		return teco_state_expectqreg_input(ctx, chr, error); \
	} \
	TECO_DEFINE_STATE(NAME, \
		.initial_cb = (teco_state_initial_cb_t)teco_state_expectqreg_initial, \
		.process_edit_cmd_cb = (teco_state_process_edit_cmd_cb_t) \
		                       teco_state_expectqreg_process_edit_cmd, \
		.insert_completion_cb = (teco_state_insert_completion_cb_t) \
		                        teco_state_expectqreg_insert_completion, \
		.style = SCE_SCITECO_QREG, \
		.expectqreg.type = TECO_QREG_REQUIRED, \
		.expectqreg.got_register_cb = NAME##_got_register, /* always required */ \
		##__VA_ARGS__ \
	)

/*
 * FIXME: Some of these states are referenced only in qreg-commands.c,
 * so they should be moved there?
 */
TECO_DECLARE_STATE(teco_state_pushqreg);
TECO_DECLARE_STATE(teco_state_popqreg);

TECO_DECLARE_STATE(teco_state_eqcommand);
TECO_DECLARE_STATE(teco_state_loadqreg);

TECO_DECLARE_STATE(teco_state_epctcommand);
TECO_DECLARE_STATE(teco_state_saveqreg);

TECO_DECLARE_STATE(teco_state_queryqreg);

TECO_DECLARE_STATE(teco_state_ctlucommand);
TECO_DECLARE_STATE(teco_state_setqregstring_nobuilding);
TECO_DECLARE_STATE(teco_state_eucommand);
TECO_DECLARE_STATE(teco_state_setqregstring_building);

TECO_DECLARE_STATE(teco_state_getqregstring);
TECO_DECLARE_STATE(teco_state_setqreginteger);
TECO_DECLARE_STATE(teco_state_increaseqreg);

TECO_DECLARE_STATE(teco_state_macro);
TECO_DECLARE_STATE(teco_state_macrofile);

TECO_DECLARE_STATE(teco_state_copytoqreg);
