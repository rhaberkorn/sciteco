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
#include "string-utils.h"

/*
 * FIXME: Introducing a second error quark might be useful to distinguish
 * errors that can be cought by SciTECO macros from errors that must always
 * propagate (TECO_ERROR_QUIT, TECO_ERROR_RETURN).
 * On the other hand, these error codes will probably soon become obsolete
 * when the SciTECO call stack no longer corresponds with the C callstack.
 */
#define TECO_ERROR (g_quark_from_static_string("sciteco-error-quark"))

typedef enum {
	/** Default (catch-all) error code */
	TECO_ERROR_FAILED = 0,

	/*
	 * FIXME: Subsume all these errors under TECO_ERROR_SYNTAX or TECO_ERROR_FAIL?
	 * They will mainly be different in their error message.
	 */
	TECO_ERROR_SYNTAX,
	TECO_ERROR_ARGEXPECTED,
	TECO_ERROR_CODEPOINT,
	TECO_ERROR_MOVE,
	TECO_ERROR_WORDS,
	TECO_ERROR_RANGE,
	TECO_ERROR_INVALIDQREG,
	TECO_ERROR_QREGOPUNSUPPORTED,
	TECO_ERROR_QREGCONTAINSNULL,
	TECO_ERROR_MEMLIMIT,

	/** Interrupt current operation */
	TECO_ERROR_INTERRUPTED,

	/** Thrown to signal command line replacement */
	TECO_ERROR_CMDLINE = 0x80,
	/** Thrown as exception to cause a macro to return or a command-line termination. */
	TECO_ERROR_RETURN,
	/** Thrown as exception to signify that program should be terminated. */
	TECO_ERROR_QUIT
} teco_error_t;

static inline void
teco_error_syntax_set(GError **error, gunichar chr)
{
	g_set_error(error, TECO_ERROR, TECO_ERROR_SYNTAX,
	            "Syntax error \"%C\" (U+%04" G_GINT32_MODIFIER "X)", chr, chr);
}

static inline void
teco_error_argexpected_set(GError **error, const gchar *cmd)
{
	g_set_error(error, TECO_ERROR, TECO_ERROR_ARGEXPECTED,
	            "Argument expected for <%s>", cmd);
}

static inline void
teco_error_codepoint_set(GError **error, const gchar *cmd)
{
	g_set_error(error, TECO_ERROR, TECO_ERROR_CODEPOINT,
	            "Invalid Unicode codepoint for <%s>", cmd);
}

static inline void
teco_error_move_set(GError **error, const gchar *cmd)
{
	g_set_error(error, TECO_ERROR, TECO_ERROR_MOVE,
	            "Attempt to move pointer off page with <%s>", cmd);
}

static inline void
teco_error_words_set(GError **error, const gchar *cmd)
{
	g_set_error(error, TECO_ERROR, TECO_ERROR_MOVE,
	            "Not enough words to delete with <%s>", cmd);
}

static inline void
teco_error_range_set(GError **error, const gchar *cmd)
{
	g_set_error(error, TECO_ERROR, TECO_ERROR_RANGE,
	            "Invalid range specified for <%s>", cmd);
}

static inline void
teco_error_invalidqreg_set(GError **error, const gchar *name, gsize len, gboolean local)
{
	g_autofree gchar *name_printable = teco_string_echo(name, len);
	g_set_error(error, TECO_ERROR, TECO_ERROR_INVALIDQREG,
	            "Invalid %sQ-Register \"%s\"", local ? "local " : "", name_printable);
}

static inline void
teco_error_qregopunsupported_set(GError **error, const gchar *name, gsize len, gboolean local)
{
	g_autofree gchar *name_printable = teco_string_echo(name, len);
	g_set_error(error, TECO_ERROR, TECO_ERROR_QREGOPUNSUPPORTED,
	            "Operation unsupported on %sQ-Register \"%s\"", local ? "local " : "", name_printable);
}

static inline void
teco_error_qregcontainsnull_set(GError **error, const gchar *name, gsize len, gboolean local)
{
	g_autofree gchar *name_printable = teco_string_echo(name, len);
	g_set_error(error, TECO_ERROR, TECO_ERROR_QREGCONTAINSNULL,
	            "%sQ-Register \"%s\" contains null-byte", local ? "Local " : "", name_printable);
}

static inline void
teco_error_interrupted_set(GError **error)
{
	g_set_error_literal(error, TECO_ERROR, TECO_ERROR_INTERRUPTED, "Interrupted");
}

extern guint teco_error_return_args;

static inline void
teco_error_return_set(GError **error, guint args)
{
	teco_error_return_args = args;
	g_set_error_literal(error, TECO_ERROR, TECO_ERROR_RETURN, "");
}

extern guint teco_error_pos, teco_error_line, teco_error_column;

void teco_error_set_coord(const gchar *str, guint pos);

void teco_error_display_short(const GError *error);
void teco_error_display_full(const GError *error);

void teco_error_add_frame_qreg(const gchar *name, gsize len);
void teco_error_add_frame_file(const gchar *name);
void teco_error_add_frame_edhook(const gchar *type);
void teco_error_add_frame_toplevel(void);

void teco_error_clear_frames(void);
