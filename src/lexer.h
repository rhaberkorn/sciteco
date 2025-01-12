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

#include "view.h"

/** Scintilla style ids for lexing SciTECO code */
typedef enum {
	SCE_SCITECO_DEFAULT	= 0,
	SCE_SCITECO_COMMAND	= 1,
	SCE_SCITECO_OPERATOR	= 2,
	SCE_SCITECO_QREG	= 3,
	SCE_SCITECO_STRING	= 4,
	SCE_SCITECO_NUMBER	= 5,
	SCE_SCITECO_LABEL	= 6,
	SCE_SCITECO_COMMENT	= 7,
	SCE_SCITECO_INVALID	= 8
} teco_style_t;

void teco_lexer_style(teco_view_t *view, gsize end);
