/*
 * Copyright (C) 2012-2023 Robin Haberkorn
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
#include "undo.h"

typedef struct {
	/**
	 * State machine used for interactive mode (commandline macro).
	 * It is initialized on-demand in main.c.
	 * This is a global variable instead of being passed down the call stack
	 * since some process_edit_cmd_cb will be for nested state machines
	 * but we must "step" only the toplevel state machine.
	 */
	teco_machine_main_t machine;

	/**
	 * String containing the current command line
	 * (both effective and rubbed out).
	 */
	teco_string_t str;
	/**
	 * Effective command line length.
	 * The length of the rubbed out part of the command line
	 * is (teco_cmdline.str.len - teco_cmdline.effective_len).
	 */
	gsize effective_len;

	/** Program counter within the command-line macro */
	guint pc;

	/**
	 * Specifies whether the immediate editing modifier
	 * is enabled/disabled.
	 * It can be toggled with the ^G immediate editing command
	 * and influences the undo/redo direction and function of the
	 * TAB key.
	 */
	gboolean modifier_enabled;
} teco_cmdline_t;

extern teco_cmdline_t teco_cmdline;

gboolean teco_cmdline_insert(const gchar *data, gsize len, GError **error);

static inline gboolean
teco_cmdline_rubin(GError **error)
{
	return teco_cmdline.effective_len >= teco_cmdline.str.len ||
	       teco_cmdline_insert(teco_cmdline.str.data + teco_cmdline.effective_len, 1, error);
}

gboolean teco_cmdline_keypress_c(gchar key, GError **error);

static inline gboolean
teco_cmdline_keypress(const gchar *str, gsize len, GError **error)
{
	for (guint i = 0; i < len; i++)
		if (!teco_cmdline_keypress_c(str[i], error))
			return FALSE;
	return TRUE;
}

gboolean teco_cmdline_fnmacro(const gchar *name, GError **error);

static inline void
teco_cmdline_rubout(void)
{
	if (teco_cmdline.effective_len)
		teco_undo_pop(--teco_cmdline.effective_len);
}

extern gboolean teco_quit_requested;

/*
 * Command states
 */

TECO_DECLARE_STATE(teco_state_save_cmdline);
