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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib.h>

#include <Scintilla.h>

#include "sciteco.h"
#include "view.h"
#include "undo.h"
#include "qreg.h"
#include "doc.h"

static inline teco_doc_scintilla_t *
teco_doc_get_scintilla(teco_doc_t *ctx)
{
	if (G_UNLIKELY(!ctx->doc))
		ctx->doc = (teco_doc_scintilla_t *)teco_view_ssm(teco_qreg_view, SCI_CREATEDOCUMENT, 0, 0);
	return ctx->doc;
}

/** @memberof teco_doc_t */
void
teco_doc_edit(teco_doc_t *ctx)
{
	teco_view_ssm(teco_qreg_view, SCI_SETDOCPOINTER, 0,
	              (sptr_t)teco_doc_get_scintilla(ctx));
	teco_view_ssm(teco_qreg_view, SCI_SETFIRSTVISIBLELINE, ctx->first_line, 0);
	teco_view_ssm(teco_qreg_view, SCI_SETXOFFSET, ctx->xoffset, 0);
	teco_view_ssm(teco_qreg_view, SCI_SETSEL, ctx->anchor, (sptr_t)ctx->dot);

	/*
	 * NOTE: Thanks to a custom Scintilla patch, se representations
	 * do not get reset after SCI_SETDOCPOINTER, so they have to be
	 * initialized only once.
	 */
	//teco_view_set_representations(teco_qreg_view);
}

/** @memberof teco_doc_t */
void
teco_doc_undo_edit(teco_doc_t *ctx)
{
	/*
	 * NOTE: see above in teco_doc_edit()
	 */
	//undo__teco_view_set_representations(teco_qreg_view);

	undo__teco_view_ssm(teco_qreg_view, SCI_SETSEL, ctx->anchor, (sptr_t)ctx->dot);
	undo__teco_view_ssm(teco_qreg_view, SCI_SETXOFFSET, ctx->xoffset, 0);
	undo__teco_view_ssm(teco_qreg_view, SCI_SETFIRSTVISIBLELINE, ctx->first_line, 0);
	undo__teco_view_ssm(teco_qreg_view, SCI_SETDOCPOINTER, 0,
	               (sptr_t)teco_doc_get_scintilla(ctx));
}

/** @memberof teco_doc_t */
void
teco_doc_set_string(teco_doc_t *ctx, const gchar *str, gsize len)
{
	if (teco_qreg_current)
		teco_doc_update(&teco_qreg_current->string, teco_qreg_view);

	teco_doc_reset(ctx);
	teco_doc_edit(ctx);

	teco_view_ssm(teco_qreg_view, SCI_BEGINUNDOACTION, 0, 0);
	teco_view_ssm(teco_qreg_view, SCI_CLEARALL, 0, 0);
	teco_view_ssm(teco_qreg_view, SCI_APPENDTEXT, len, (sptr_t)(str ? : ""));
	teco_view_ssm(teco_qreg_view, SCI_ENDUNDOACTION, 0, 0);

	if (teco_qreg_current)
		teco_doc_edit(&teco_qreg_current->string);
}

/** @memberof teco_doc_t */
void
teco_doc_undo_set_string(teco_doc_t *ctx)
{
	/*
	 * Necessary, so that upon rubout the
	 * string's parameters are restored.
	 */
	teco_doc_update(ctx, teco_qreg_view);

	if (teco_qreg_current && teco_qreg_current->must_undo) // FIXME
		teco_doc_undo_edit(&teco_qreg_current->string);

	teco_doc_undo_reset(ctx);
	undo__teco_view_ssm(teco_qreg_view, SCI_UNDO, 0, 0);

	teco_doc_undo_edit(ctx);
}

/**
 * Get a document as a string.
 *
 * @param ctx The document.
 * @param str Pointer to a variable to hold the return string.
 *            It can be NULL if you are interested only in the string's length.
 *            Strings must be freed via g_free().
 * @param len Where to store the string's length (mandatory).
 *
 * @see teco_qreg_vtable_t::get_string()
 * @memberof teco_doc_t
 */
void
teco_doc_get_string(teco_doc_t *ctx, gchar **str, gsize *len)
{
	if (!ctx->doc) {
		if (str)
			*str = NULL;
		*len = 0;
		return;
	}

	if (teco_qreg_current)
		teco_doc_update(&teco_qreg_current->string, teco_qreg_view);

	teco_doc_edit(ctx);

	*len = teco_view_ssm(teco_qreg_view, SCI_GETLENGTH, 0, 0);
	if (str) {
		*str = g_malloc(*len + 1);
		teco_view_ssm(teco_qreg_view, SCI_GETTEXT, *len + 1, (sptr_t)*str);
	}

	if (teco_qreg_current)
		teco_doc_edit(&teco_qreg_current->string);
}

/** @memberof teco_doc_t */
void
teco_doc_update_from_view(teco_doc_t *ctx, teco_view_t *from)
{
	ctx->anchor = teco_view_ssm(from, SCI_GETANCHOR, 0, 0);
	ctx->dot = teco_view_ssm(from, SCI_GETCURRENTPOS, 0, 0);
	ctx->first_line = teco_view_ssm(from, SCI_GETFIRSTVISIBLELINE, 0, 0);
	ctx->xoffset = teco_view_ssm(from, SCI_GETXOFFSET, 0, 0);
}

/** @memberof teco_doc_t */
void
teco_doc_update_from_doc(teco_doc_t *ctx, const teco_doc_t *from)
{
	ctx->anchor = from->anchor;
	ctx->dot = from->dot;
	ctx->first_line = from->first_line;
	ctx->xoffset = from->xoffset;
}

/**
 * Only for teco_qreg_stack_pop() which does some clever
 * exchanging of document data (without any deep copying)
 *
 * @memberof teco_doc_t
 */
void
teco_doc_exchange(teco_doc_t *ctx, teco_doc_t *other)
{
	teco_doc_t temp;
	memcpy(&temp, ctx, sizeof(temp));
	memcpy(ctx, other, sizeof(*ctx));
	memcpy(other, &temp, sizeof(*other));
}

/** @memberof teco_doc_t */
void
teco_doc_clear(teco_doc_t *ctx)
{
	if (ctx->doc)
		teco_view_ssm(teco_qreg_view, SCI_RELEASEDOCUMENT, 0, (sptr_t)ctx->doc);
}
