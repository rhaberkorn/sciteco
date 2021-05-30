/*
 * Copyright (C) 2012-2021 Robin Haberkorn
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

#include "sciteco.h"
#include "parser.h"

typedef struct {
	GFileTest test;
	gchar *dirname;
	GDir *dir;
	GRegex *pattern;
} teco_globber_t;

void teco_globber_init(teco_globber_t *ctx, const gchar *pattern, GFileTest test);
gchar *teco_globber_next(teco_globber_t *ctx);
void teco_globber_clear(teco_globber_t *ctx);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(teco_globber_t, teco_globber_clear);

/** @static @memberof teco_globber_t */
static inline gboolean
teco_globber_is_pattern(const gchar *str)
{
	return str && strpbrk(str, "*?[") != NULL;
}

gchar *teco_globber_escape_pattern(const gchar *pattern);
GRegex *teco_globber_compile_pattern(const gchar *pattern);

/*
 * Command states
 */

TECO_DECLARE_STATE(teco_state_glob_pattern);
