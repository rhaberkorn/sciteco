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

void teco_state_start_typeout(teco_machine_main_t *ctx, GError **error);
void teco_state_control_typeout(teco_machine_main_t *ctx, GError **error);

/*
 * Command states
 */
TECO_DECLARE_STATE(teco_state_print_decimal);
TECO_DECLARE_STATE(teco_state_print_string);
