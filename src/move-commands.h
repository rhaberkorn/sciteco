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

#include "parser.h"

void teco_state_start_jump(teco_machine_main_t *ctx, GError **error);

void teco_state_start_move(teco_machine_main_t *ctx, GError **error);
void teco_state_start_reverse(teco_machine_main_t *ctx, GError **error);

void teco_state_start_line(teco_machine_main_t *ctx, GError **error);
void teco_state_start_back(teco_machine_main_t *ctx, GError **error);

void teco_state_start_word(teco_machine_main_t *ctx, GError **error);

void teco_state_start_delete_words(teco_machine_main_t *ctx, GError **error);
void teco_state_start_delete_words_back(teco_machine_main_t *ctx, GError **error);

void teco_state_start_kill_lines(teco_machine_main_t *ctx, GError **error);
void teco_state_start_delete_chars(teco_machine_main_t *ctx, GError **error);

void teco_state_control_lines2glyphs(teco_machine_main_t *ctx, GError **error);
