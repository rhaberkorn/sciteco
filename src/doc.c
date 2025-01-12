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
teco_doc_scintilla_ref(teco_doc_scintilla_t *doc)
{
	if (doc)
		teco_view_ssm(teco_qreg_view, SCI_ADDREFDOCUMENT, 0, (sptr_t)doc);
	return doc;
}

static inline void
teco_doc_scintilla_release(teco_doc_scintilla_t *doc)
{
	if (doc)
		teco_view_ssm(teco_qreg_view, SCI_RELEASEDOCUMENT, 0, (sptr_t)doc);
}

TECO_DEFINE_UNDO_OBJECT(doc_scintilla, teco_doc_scintilla_t *,
                        teco_doc_scintilla_ref, teco_doc_scintilla_release);

static inline teco_doc_scintilla_t *
teco_doc_get_scintilla(teco_doc_t *ctx)
{
	/*
	 * FIXME: Perhaps we should always specify SC_DOCUMENTOPTION_TEXT_LARGE?
	 * SC_DOCUMENTOPTION_STYLES_NONE is unfortunately also not safe to set
	 * always as the Q-Reg might well be used for styling even in batch mode.
	 */
	if (G_UNLIKELY(!ctx->doc))
		ctx->doc = (teco_doc_scintilla_t *)teco_view_ssm(teco_qreg_view, SCI_CREATEDOCUMENT, 0, 0);
	return ctx->doc;
}

/**
 * Edit the given document in the Q-Register view.
 *
 * @param ctx The document to edit.
 * @param default_cp The codepage to configure if the document is new.
 *
 * @memberof teco_doc_t
 */
void
teco_doc_edit(teco_doc_t *ctx, guint default_cp)
{
	gboolean new_doc = ctx->doc == NULL;

	teco_view_ssm(teco_qreg_view, SCI_SETDOCPOINTER, 0,
	              (sptr_t)teco_doc_get_scintilla(ctx));
	teco_view_ssm(teco_qreg_view, SCI_SETFIRSTVISIBLELINE, ctx->first_line, 0);
	teco_view_ssm(teco_qreg_view, SCI_SETXOFFSET, ctx->xoffset, 0);
	teco_view_ssm(teco_qreg_view, SCI_SETSEL, ctx->anchor, (sptr_t)ctx->dot);

	/*
	 * NOTE: Thanks to a custom Scintilla patch, representations
	 * do not get reset after SCI_SETDOCPOINTER, so they have to be
	 * initialized only once.
	 */
	//teco_view_set_representations(teco_qreg_view);

	if (new_doc && default_cp != SC_CP_UTF8) {
		/*
		 * There is a chance the user will see this buffer even if we
		 * are currently in batch mode.
		 */
		for (gint style = 0; style <= STYLE_LASTPREDEFINED; style++)
			teco_view_ssm(teco_qreg_view, SCI_STYLESETCHARACTERSET,
			              style, default_cp);
		/* 0 is used for ALL single-byte encodings */
		teco_view_ssm(teco_qreg_view, SCI_SETCODEPAGE, 0, 0);
	} else if (!(teco_view_ssm(teco_qreg_view, SCI_GETLINECHARACTERINDEX, 0, 0)
							& SC_LINECHARACTERINDEX_UTF32)) {
		/*
		 * All UTF-8 documents are expected to have a character index.
		 * This allocates nothing if the document is not UTF-8.
		 * But it is reference counted, so it must not be allocated
		 * more than once.
		 *
		 * FIXME: This apparently gets reset with every SCI_SETDOCPOINTER
		 * (although I don't know why and where).
		 * Recalculating it could be inefficient.
		 * The index is reference-counted. Perhaps we could just allocate
		 * one more time, so it doesn't get freed when changing documents.
		 */
		teco_view_ssm(teco_qreg_view, SCI_ALLOCATELINECHARACTERINDEX,
		              SC_LINECHARACTERINDEX_UTF32, 0);
	}
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
teco_doc_set_string(teco_doc_t *ctx, const gchar *str, gsize len, guint codepage)
{
	if (teco_qreg_current)
		teco_doc_update(&teco_qreg_current->string, teco_qreg_view);

	teco_doc_scintilla_release(ctx->doc);
	ctx->doc = NULL;

	teco_doc_reset(ctx);
	teco_doc_edit(ctx, codepage);

	teco_view_ssm(teco_qreg_view, SCI_APPENDTEXT, len, (sptr_t)(str ? : ""));

	if (teco_qreg_current)
		teco_doc_edit(&teco_qreg_current->string, 0);
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

	if (teco_qreg_current && teco_qreg_current->must_undo && // FIXME
	    ctx == &teco_qreg_current->string)
		/* load old document into view */
		teco_doc_undo_edit(&teco_qreg_current->string);

	teco_doc_undo_reset(ctx);
	teco_undo_object_doc_scintilla_push(&ctx->doc);
}

/**
 * Get a document as a string.
 *
 * @param ctx The document.
 * @param str Pointer to a variable to hold the return string.
 *            It can be NULL if you are interested only in the string's length.
 *            Strings must be freed via g_free().
 * @param len Where to store the string's length or NULL
 *            if that information is not necessary.
 * @param codepage Where to store the document's codepage or NULL
 *                 if that information is not necessary.
 *
 * @see teco_qreg_vtable_t::get_string()
 * @memberof teco_doc_t
 */
void
teco_doc_get_string(teco_doc_t *ctx, gchar **str, gsize *outlen, guint *codepage)
{
	if (!ctx->doc) {
		if (str)
			*str = NULL;
		if (outlen)
			*outlen = 0;
		if (codepage)
			*codepage = teco_default_codepage();
		return;
	}

	if (teco_qreg_current)
		teco_doc_update(&teco_qreg_current->string, teco_qreg_view);

	teco_doc_edit(ctx, teco_default_codepage());

	gsize len = teco_view_ssm(teco_qreg_view, SCI_GETLENGTH, 0, 0);
	if (str) {
		*str = g_malloc(len + 1);
		teco_view_ssm(teco_qreg_view, SCI_GETTEXT, len + 1, (sptr_t)*str);
	}
	if (outlen)
		*outlen = len;
	if (codepage)
		*codepage = teco_view_get_codepage(teco_qreg_view);

	if (teco_qreg_current)
		teco_doc_edit(&teco_qreg_current->string, 0);
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
	teco_doc_scintilla_release(ctx->doc);
}
