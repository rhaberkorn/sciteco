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
#include <gmodule.h>

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
	TECO_ERROR_MODIFIER,
	TECO_ERROR_ARGEXPECTED,
	TECO_ERROR_CODEPOINT,
	TECO_ERROR_MOVE,
	TECO_ERROR_WORDS,
	TECO_ERROR_RANGE,
	TECO_ERROR_SUBPATTERN,
	TECO_ERROR_INVALIDBUF,
	TECO_ERROR_INVALIDQREG,
	TECO_ERROR_QREGOPUNSUPPORTED,
	TECO_ERROR_QREGCONTAINSNULL,
	TECO_ERROR_EDITINGLOCALQREG,
	TECO_ERROR_MEMLIMIT,
	TECO_ERROR_CLIPBOARD,
	TECO_ERROR_WIN32,
	TECO_ERROR_MODULE,

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
	gchar buf[6];
	g_autofree gchar *chr_printable = teco_string_echo(buf, g_unichar_to_utf8(chr, buf));
	g_set_error(error, TECO_ERROR, TECO_ERROR_SYNTAX,
	            "Syntax error \"%s\" (U+%04" G_GINT32_MODIFIER "X)", chr_printable, chr);
}

static inline void
teco_error_modifier_set(GError **error, gchar chr)
{
	g_set_error(error, TECO_ERROR, TECO_ERROR_MODIFIER,
	            "Unexpected modifier on <%c>", chr);
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
	g_set_error(error, TECO_ERROR, TECO_ERROR_WORDS,
	            "Not enough words to perform <%s>", cmd);
}

static inline void
teco_error_range_set(GError **error, const gchar *cmd)
{
	g_set_error(error, TECO_ERROR, TECO_ERROR_RANGE,
	            "Invalid range specified for <%s>", cmd);
}

static inline void
teco_error_subpattern_set(GError **error, const gchar *cmd)
{
	g_set_error(error, TECO_ERROR, TECO_ERROR_SUBPATTERN,
	            "Invalid subpattern specified for <%s>", cmd);
}

static inline void
teco_error_invalidbuf_set(GError **error, teco_int_t id)
{
	g_set_error(error, TECO_ERROR, TECO_ERROR_INVALIDBUF,
	            "Invalid buffer id %" TECO_INT_FORMAT, id);
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
teco_error_editinglocalqreg_set(GError **error, const gchar *name, gsize len)
{
	g_autofree gchar *name_printable = teco_string_echo(name, len);
	g_set_error(error, TECO_ERROR, TECO_ERROR_EDITINGLOCALQREG,
	            "Editing local Q-Register \"%s\" at end of macro call", name_printable);
}

#ifdef G_OS_WIN32
static inline void
teco_error_win32_set(GError **error, const gchar *prefix, gint err)
{
	g_autofree gchar *msg = g_win32_error_message(err);
	g_set_error(error, TECO_ERROR, TECO_ERROR_WIN32, "%s: %s", prefix, msg);
}
#endif

static inline void
teco_error_module_set(GError **error, const gchar *prefix)
{
	g_set_error(error, TECO_ERROR, TECO_ERROR_MODULE, "%s: %s",
	            prefix, g_module_error());
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

static inline void
teco_error_set_coord(const gchar *str, gsize pos)
{
	teco_string_get_coord(str, pos, &teco_error_pos, &teco_error_line, &teco_error_column);
}

void teco_error_display_short(const GError *error);
void teco_error_display_full(const GError *error);

void teco_error_add_frame_qreg(const gchar *name, gsize len);
void teco_error_add_frame_file(const gchar *name);
void teco_error_add_frame_edhook(const gchar *type);
void teco_error_add_frame_toplevel(void);

void teco_error_clear_frames(void);
