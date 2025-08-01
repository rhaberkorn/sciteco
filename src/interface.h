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

#include <signal.h>

#include <glib.h>

#include <Scintilla.h>

#include "sciteco.h"
#include "qreg.h"
#include "ring.h"
#include "cmdline.h"
#include "view.h"

/**
 * @file
 * Abstract user interface.
 *
 * Interface of all SciTECO user interfaces (e.g. Curses or GTK+).
 * All functions that must be provided are marked with the \@pure tag.
 *
 * @note
 * We do not provide default implementations for any of the interface
 * functions by declaring them "weak" since this is a non-portable linker
 * feature.
 */

/**
 * Interval between polling for keypresses (if necessary).
 * In other words, this is the maximum latency to detect CTRL+C interruptions.
 */
#define TECO_POLL_INTERVAL 100000 /* microseconds */

/** @protected */
extern teco_view_t *teco_interface_current_view;

/** @pure */
void teco_interface_init(void);

/** @pure */
GOptionGroup *teco_interface_get_options(void);

/** @pure makes sense only on Curses */
void teco_interface_init_color(guint color, guint32 rgb);

typedef enum {
	TECO_MSG_USER,
	TECO_MSG_INFO,
	TECO_MSG_WARNING,
	TECO_MSG_ERROR
} teco_msg_t;

/** @pure */
void teco_interface_msg_literal(teco_msg_t type, const gchar *str, gsize len);

void teco_interface_msg(teco_msg_t type, const gchar *fmt, ...) G_GNUC_PRINTF(2, 3);

/** @pure */
teco_int_t teco_interface_getch(gboolean widechar);

/** @pure */
void teco_interface_msg_clear(void);

/** @pure */
void teco_interface_show_view(teco_view_t *view);
void undo__teco_interface_show_view(teco_view_t *);

static inline sptr_t
teco_interface_ssm(unsigned int iMessage, uptr_t wParam, sptr_t lParam)
{
	return teco_view_ssm(teco_interface_current_view, iMessage, wParam, lParam);
}

/*
 * NOTE: You could simply call undo__teco_view_ssm(teco_interface_current_view, ...).
 * undo__teco_interface_ssm(...) exists for brevity and aestethics.
 */
void undo__teco_interface_ssm(unsigned int, uptr_t, sptr_t);

/** Expand folds, so that dot is always visible. */
static inline void
teco_interface_unfold(void)
{
	sptr_t dot = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);
	teco_interface_ssm(SCI_ENSUREVISIBLE, teco_interface_ssm(SCI_LINEFROMPOSITION, dot, 0), 0);
}

/** @pure */
void teco_interface_info_update_qreg(const teco_qreg_t *reg);
/** @pure */
void teco_interface_info_update_buffer(const teco_buffer_t *buffer);

#define teco_interface_info_update(X) \
	(_Generic((X), teco_qreg_t *         : teco_interface_info_update_qreg, \
	               const teco_qreg_t *   : teco_interface_info_update_qreg, \
	               teco_buffer_t *       : teco_interface_info_update_buffer, \
	               const teco_buffer_t * : teco_interface_info_update_buffer)(X))

void undo__teco_interface_info_update_qreg(const teco_qreg_t *);
void undo__teco_interface_info_update_buffer(const teco_buffer_t *);

/** @pure */
void teco_interface_cmdline_update(const teco_cmdline_t *cmdline);

/** @pure */
gboolean teco_interface_set_clipboard(const gchar *name, const gchar *str, gsize str_len,
                                      GError **error);
void teco_interface_undo_set_clipboard(const gchar *name, gchar *str, gsize len);
/**
 * Semantics are compatible with teco_qreg_vtable_t::get_string() since that is the
 * main user of this function.
 *
 * @pure
 */
gboolean teco_interface_get_clipboard(const gchar *name, gchar **str, gsize *len, GError **error);

typedef enum {
	TECO_POPUP_PLAIN,
	TECO_POPUP_FILE,
	TECO_POPUP_DIRECTORY
} teco_popup_entry_type_t;

/** @pure */
void teco_interface_popup_add(teco_popup_entry_type_t type,
                              const gchar *name, gsize name_len, gboolean highlight);
/** @pure */
void teco_interface_popup_show(gsize prefix_len);
/** @pure */
void teco_interface_popup_scroll(void);
/** @pure */
gboolean teco_interface_popup_is_shown(void);
/** @pure */
void teco_interface_popup_clear(void);

/** @pure */
gboolean teco_interface_is_interrupted(void);

typedef struct {
	enum {
		TECO_MOUSE_PRESSED = 1,
		TECO_MOUSE_RELEASED,
		TECO_MOUSE_SCROLLUP,
		TECO_MOUSE_SCROLLDOWN
	} type;

	guint x;	/*< X-coordinate relative to view */
	guint y;	/*< Y-coordinate relative to view */

	gint button;	/*< number of pressed mouse button or -1 */

	enum {
		TECO_MOUSE_SHIFT	= (1 << 0),
		TECO_MOUSE_CTRL		= (1 << 1),
		TECO_MOUSE_ALT		= (1 << 2)
	} mods;
} teco_mouse_t;

extern teco_mouse_t teco_mouse;

/** @pure main entry point */
gboolean teco_interface_event_loop(GError **error);

/*
 * Interfacing to the external SciTECO world
 */
/** @protected */
void teco_interface_stdio_msg(teco_msg_t type, const gchar *str, gsize len);

/** @protected */
teco_int_t teco_interface_stdio_getch(gboolean widechar);

/** @protected */
void teco_interface_refresh(gboolean force);

/** @pure */
void teco_interface_cleanup(void);

static inline guint
teco_interface_get_codepage(void)
{
	return teco_view_get_codepage(teco_interface_current_view);
}

static inline gssize
teco_interface_glyphs2bytes(teco_int_t pos)
{
	return teco_view_glyphs2bytes(teco_interface_current_view, pos);
}

static inline teco_int_t
teco_interface_bytes2glyphs(gsize pos)
{
	return teco_view_bytes2glyphs(teco_interface_current_view, pos);
}

static inline gssize
teco_interface_glyphs2bytes_relative(gsize pos, teco_int_t n)
{
	return teco_view_glyphs2bytes_relative(teco_interface_current_view, pos, n);
}

static inline teco_int_t
teco_interface_get_character(gsize pos, gsize len)
{
	return teco_view_get_character(teco_interface_current_view, pos, len);
}

/*
 * The following functions are here for lack of a better place.
 * They could also be in sciteco.h, but only if declared as non-inline
 * since sciteco.h should not depend on interface.h.
 */

static inline gboolean
teco_validate_line(teco_int_t n)
{
	return 0 <= n && n < teco_interface_ssm(SCI_GETLINECOUNT, 0, 0);
}
