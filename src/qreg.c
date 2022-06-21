/*
 * Copyright (C) 2012-2022 Robin Haberkorn
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

#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>

#include <Scintilla.h>

#include "sciteco.h"
#include "string-utils.h"
#include "file-utils.h"
#include "interface.h"
#include "cmdline.h"
#include "view.h"
#include "undo.h"
#include "parser.h"
#include "core-commands.h"
#include "expressions.h"
#include "doc.h"
#include "ring.h"
#include "eol.h"
#include "error.h"
#include "qreg.h"

/**
 * View used for editing Q-Registers.
 * Initialized in main.c after the interface.
 */
teco_view_t *teco_qreg_view = NULL;
/** Currently edited Q-Register */
teco_qreg_t *teco_qreg_current = NULL;

/**
 * Table for global Q-Registers.
 * Initialized in main.c after the interface.
 */
teco_qreg_table_t teco_qreg_table_globals;

/** @private @static @memberof teco_qreg_t */
static teco_qreg_t *
teco_qreg_new(teco_qreg_vtable_t *vtable, const gchar *name, gsize len)
{
	/*
	 * FIXME: Test with g_slice_new()...
	 * It could however cause problems upon command-line termination
	 * and may not be measurably faster.
	 */
	teco_qreg_t *qreg = g_new0(teco_qreg_t, 1);
	qreg->vtable = vtable;
	/*
	 * NOTE: This does not use GStringChunk/teco_string_init_chunk()
	 * since we want to implement Q-Register removing soon.
	 * Even without that, individual Q-Regs can be removed on rubout.
	 */
	teco_string_init(&qreg->head.name, name, len);
	teco_doc_init(&qreg->string);
	return qreg;
}

/** @memberof teco_qreg_t */
gboolean
teco_qreg_execute(teco_qreg_t *qreg, teco_qreg_table_t *qreg_table_locals, GError **error)
{
	g_auto(teco_string_t) macro = {NULL, 0};

	if (!qreg->vtable->get_string(qreg, &macro.data, &macro.len, error) ||
	    !teco_execute_macro(macro.data, macro.len, qreg_table_locals, error)) {
		teco_error_add_frame_qreg(qreg->head.name.data, qreg->head.name.len);
		return FALSE;
	}

	return TRUE;
}

/** @memberof teco_qreg_t */
void
teco_qreg_undo_set_eol_mode(teco_qreg_t *qreg)
{
	if (!qreg->must_undo)
		return;

	/*
	 * Necessary, so that upon rubout the
	 * string's parameters are restored.
	 */
	teco_doc_update(&qreg->string, teco_qreg_view);

	if (teco_qreg_current && teco_qreg_current->must_undo) // FIXME
		teco_doc_undo_edit(&teco_qreg_current->string);

	undo__teco_view_ssm(teco_qreg_view, SCI_SETEOLMODE,
	                    teco_view_ssm(teco_qreg_view, SCI_GETEOLMODE, 0, 0), 0);

	teco_doc_undo_edit(&qreg->string);
}

/** @memberof teco_qreg_t */
void
teco_qreg_set_eol_mode(teco_qreg_t *qreg, gint mode)
{
	if (teco_qreg_current)
		teco_doc_update(&teco_qreg_current->string, teco_qreg_view);

	teco_doc_edit(&qreg->string);
	teco_view_ssm(teco_qreg_view, SCI_SETEOLMODE, mode, 0);

	if (teco_qreg_current)
		teco_doc_edit(&teco_qreg_current->string);
}

/** @memberof teco_qreg_t */
gboolean
teco_qreg_load(teco_qreg_t *qreg, const gchar *filename, GError **error)
{
	if (!qreg->vtable->undo_set_string(qreg, error))
		return FALSE;

	if (teco_qreg_current)
		teco_doc_update(&teco_qreg_current->string, teco_qreg_view);

	teco_doc_edit(&qreg->string);
	teco_doc_reset(&qreg->string);

	/*
	 * teco_view_load() might change the EOL style.
	 */
	teco_qreg_undo_set_eol_mode(qreg);

	/*
	 * undo_set_string() pushes undo tokens that restore
	 * the previous document in the view.
	 * So if loading fails, teco_qreg_current will be
	 * made the current document again.
	 */
	if (!teco_view_load(teco_qreg_view, filename, error))
		return FALSE;

	if (teco_qreg_current)
		teco_doc_edit(&teco_qreg_current->string);

	return TRUE;
}

/** @memberof teco_qreg_t */
gboolean
teco_qreg_save(teco_qreg_t *qreg, const gchar *filename, GError **error)
{
	if (teco_qreg_current)
		teco_doc_update(&teco_qreg_current->string, teco_qreg_view);

	teco_doc_edit(&qreg->string);

	if (!teco_view_save(teco_qreg_view, filename, error)) {
		if (teco_qreg_current)
			teco_doc_edit(&teco_qreg_current->string);
		return FALSE;
	}

	if (teco_qreg_current)
		teco_doc_edit(&teco_qreg_current->string);

	return TRUE;
}

static gboolean
teco_qreg_plain_set_integer(teco_qreg_t *qreg, teco_int_t value, GError **error)
{
	qreg->integer = value;
	return TRUE;
}

static gboolean
teco_qreg_plain_undo_set_integer(teco_qreg_t *qreg, GError **error)
{
	if (qreg->must_undo) // FIXME
		teco_undo_int(qreg->integer);
	return TRUE;
}

static gboolean
teco_qreg_plain_get_integer(teco_qreg_t *qreg, teco_int_t *ret, GError **error)
{
	*ret = qreg->integer;
	return TRUE;
}

static gboolean
teco_qreg_plain_set_string(teco_qreg_t *qreg, const gchar *str, gsize len, GError **error)
{
	teco_doc_set_string(&qreg->string, str, len);
	return TRUE;
}

static gboolean
teco_qreg_plain_undo_set_string(teco_qreg_t *qreg, GError **error)
{
	if (qreg->must_undo) // FIXME
		teco_doc_undo_set_string(&qreg->string);
	return TRUE;
}

static gboolean
teco_qreg_plain_append_string(teco_qreg_t *qreg, const gchar *str, gsize len, GError **error)
{
	/*
	 * NOTE: Will not create undo action if string is empty.
	 * Also, appending preserves the string's parameters.
	 */
	if (!len)
		return TRUE;

	if (teco_qreg_current)
		teco_doc_update(&teco_qreg_current->string, teco_qreg_view);

	teco_doc_edit(&qreg->string);

	teco_view_ssm(teco_qreg_view, SCI_BEGINUNDOACTION, 0, 0);
	teco_view_ssm(teco_qreg_view, SCI_APPENDTEXT, len, (sptr_t)str);
	teco_view_ssm(teco_qreg_view, SCI_ENDUNDOACTION, 0, 0);

	if (teco_qreg_current)
		teco_doc_edit(&teco_qreg_current->string);
	return TRUE;
}

static gboolean
teco_qreg_plain_get_string(teco_qreg_t *qreg, gchar **str, gsize *len, GError **error)
{
	teco_doc_get_string(&qreg->string, str, len);
	return TRUE;
}

static gint
teco_qreg_plain_get_character(teco_qreg_t *qreg, guint position, GError **error)
{
	gint ret = -1;

	if (teco_qreg_current)
		teco_doc_update(&teco_qreg_current->string, teco_qreg_view);

	teco_doc_edit(&qreg->string);

	if (position < teco_view_ssm(teco_qreg_view, SCI_GETLENGTH, 0, 0))
		ret = teco_view_ssm(teco_qreg_view, SCI_GETCHARAT, position, 0);
	else
		g_set_error(error, TECO_ERROR, TECO_ERROR_RANGE,
		            "Position %u out of range", position);
		/* make sure we still restore the current Q-Register */

	if (teco_qreg_current)
		teco_doc_edit(&teco_qreg_current->string);

	return ret;
}

static gboolean
teco_qreg_plain_exchange_string(teco_qreg_t *qreg, teco_doc_t *src, GError **error)
{
	teco_doc_exchange(&qreg->string, src);
	return TRUE;
}

static gboolean
teco_qreg_plain_undo_exchange_string(teco_qreg_t *qreg, teco_doc_t *src, GError **error)
{
	if (qreg->must_undo) // FIXME
		teco_doc_undo_exchange(&qreg->string);
	teco_doc_undo_exchange(src);
	return TRUE;
}

static gboolean
teco_qreg_plain_edit(teco_qreg_t *qreg, GError **error)
{
	if (teco_qreg_current)
		teco_doc_update(&teco_qreg_current->string, teco_qreg_view);

	teco_doc_edit(&qreg->string);
	teco_interface_show_view(teco_qreg_view);
	teco_interface_info_update(qreg);

	return TRUE;
}

static gboolean
teco_qreg_plain_undo_edit(teco_qreg_t *qreg, GError **error)
{
	/*
	 * We might be switching the current document
	 * to a buffer.
	 */
	teco_doc_update(&qreg->string, teco_qreg_view);

	if (!qreg->must_undo) // FIXME
		return TRUE;

	undo__teco_interface_info_update_qreg(qreg);
	teco_doc_undo_edit(&qreg->string);
	undo__teco_interface_show_view(teco_qreg_view);
	return TRUE;
}

#define TECO_INIT_QREG(...) { \
	.set_integer		= teco_qreg_plain_set_integer, \
	.undo_set_integer	= teco_qreg_plain_undo_set_integer, \
	.get_integer		= teco_qreg_plain_get_integer, \
	.set_string		= teco_qreg_plain_set_string, \
	.undo_set_string	= teco_qreg_plain_undo_set_string, \
	.append_string		= teco_qreg_plain_append_string, \
	.undo_append_string	= teco_qreg_plain_undo_set_string, \
	.get_string		= teco_qreg_plain_get_string, \
	.get_character		= teco_qreg_plain_get_character, \
	.exchange_string	= teco_qreg_plain_exchange_string, \
	.undo_exchange_string	= teco_qreg_plain_undo_exchange_string, \
	.edit			= teco_qreg_plain_edit, \
	.undo_edit		= teco_qreg_plain_undo_edit, \
	##__VA_ARGS__ \
}

/** @static @memberof teco_qreg_t */
teco_qreg_t *
teco_qreg_plain_new(const gchar *name, gsize len)
{
	static teco_qreg_vtable_t vtable = TECO_INIT_QREG();

	return teco_qreg_new(&vtable, name, len);
}

/*
 * NOTE: The integer-component is currently unused on the "*" special register.
 */
static gboolean
teco_qreg_bufferinfo_set_integer(teco_qreg_t *qreg, teco_int_t value, GError **error)
{
	return teco_ring_edit(value, error);
}

static gboolean
teco_qreg_bufferinfo_undo_set_integer(teco_qreg_t *qreg, GError **error)
{
	return teco_current_doc_undo_edit(error);
}

static gboolean
teco_qreg_bufferinfo_get_integer(teco_qreg_t *qreg, teco_int_t *ret, GError **error)
{
	*ret = teco_ring_get_id(teco_ring_current);
	return TRUE;
}

/*
 * FIXME: These operations can and should be implemented.
 * Setting the "*" register could for instance rename the file.
 */
static gboolean
teco_qreg_bufferinfo_set_string(teco_qreg_t *qreg, const gchar *str, gsize len, GError **error)
{
	teco_error_qregopunsupported_set(error, qreg->head.name.data, qreg->head.name.len, FALSE);
	return FALSE;
}

static gboolean
teco_qreg_bufferinfo_undo_set_string(teco_qreg_t *qreg, GError **error)
{
	return TRUE;
}

static gboolean
teco_qreg_bufferinfo_append_string(teco_qreg_t *qreg, const gchar *str, gsize len, GError **error)
{
	teco_error_qregopunsupported_set(error, qreg->head.name.data, qreg->head.name.len, FALSE);
	return FALSE;
}

static gboolean
teco_qreg_bufferinfo_undo_append_string(teco_qreg_t *qreg, GError **error)
{
	return TRUE;
}

/*
 * NOTE: The `string` component is currently unused on the "*" register.
 */
static gboolean
teco_qreg_bufferinfo_get_string(teco_qreg_t *qreg, gchar **str, gsize *len, GError **error)
{
	/*
	 * On platforms with a default non-forward-slash directory
	 * separator (i.e. Windows), Buffer::filename will have
	 * the wrong separator.
	 * To make the life of macros that evaluate "*" easier,
	 * the directory separators are normalized to "/" here.
	 */
	if (str)
		*str = teco_file_normalize_path(g_strdup(teco_ring_current->filename ? : ""));
	/*
	 * NOTE: teco_file_normalize_path() does not change the size of the string.
	 */
	*len = teco_ring_current->filename ? strlen(teco_ring_current->filename) : 0;
	return TRUE;
}

static gint
teco_qreg_bufferinfo_get_character(teco_qreg_t *qreg, guint position, GError **error)
{
	gsize max_len;

	if (!teco_qreg_bufferinfo_get_string(qreg, NULL, &max_len, error))
		return -1;

	if (position >= max_len) {
		g_set_error(error, TECO_ERROR, TECO_ERROR_RANGE,
		            "Position %u out of range", position);
		return -1;
	}

	return teco_ring_current->filename[position];
}

static gboolean
teco_qreg_bufferinfo_edit(teco_qreg_t *qreg, GError **error)
{
	if (!teco_qreg_plain_edit(qreg, error))
		return FALSE;

	g_auto(teco_string_t) str = {NULL, 0};

	if (!teco_qreg_bufferinfo_get_string(qreg, &str.data, &str.len, error))
		return FALSE;

	teco_view_ssm(teco_qreg_view, SCI_BEGINUNDOACTION, 0, 0);
	teco_view_ssm(teco_qreg_view, SCI_CLEARALL, 0, 0);
	teco_view_ssm(teco_qreg_view, SCI_ADDTEXT, str.len, (sptr_t)str.data);
	teco_view_ssm(teco_qreg_view, SCI_ENDUNDOACTION, 0, 0);

	undo__teco_view_ssm(teco_qreg_view, SCI_UNDO, 0, 0);
	return TRUE;
}

/** @static @memberof teco_qreg_t */
teco_qreg_t *
teco_qreg_bufferinfo_new(void)
{
	static teco_qreg_vtable_t vtable = TECO_INIT_QREG(
		.set_integer		= teco_qreg_bufferinfo_set_integer,
		.undo_set_integer	= teco_qreg_bufferinfo_undo_set_integer,
		.get_integer		= teco_qreg_bufferinfo_get_integer,
		.set_string		= teco_qreg_bufferinfo_set_string,
		.undo_set_string	= teco_qreg_bufferinfo_undo_set_string,
		.append_string		= teco_qreg_bufferinfo_append_string,
		.undo_append_string	= teco_qreg_bufferinfo_undo_append_string,
		.get_string		= teco_qreg_bufferinfo_get_string,
		.get_character		= teco_qreg_bufferinfo_get_character,
		.edit			= teco_qreg_bufferinfo_edit
	);

	return teco_qreg_new(&vtable, "*", 1);
}

static gboolean
teco_qreg_workingdir_set_string(teco_qreg_t *qreg, const gchar *str, gsize len, GError **error)
{
	/*
	 * NOTE: Makes sure that `dir` will be null-terminated as str[len] may not be '\0'.
	 */
	g_auto(teco_string_t) dir;
	teco_string_init(&dir, str, len);

	if (teco_string_contains(&dir, '\0')) {
		g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
		                    "Directory contains null-character");
		return FALSE;
	}

	int ret = g_chdir(dir.data);
	if (ret) {
		/* FIXME: Is errno usable on Windows here? */
		g_set_error(error, TECO_ERROR, TECO_ERROR_FAILED,
		            "Cannot change working directory to \"%s\"", dir.data);
		return FALSE;
	}

	return TRUE;
}

static gboolean
teco_qreg_workingdir_undo_set_string(teco_qreg_t *qreg, GError **error)
{
	teco_undo_change_dir_to_current();
	return TRUE;
}

/*
 * FIXME: Redundant with teco_qreg_bufferinfo_append_string()...
 * Best solution would be to simply implement them.
 */
static gboolean
teco_qreg_workingdir_append_string(teco_qreg_t *qreg, const gchar *str, gsize len, GError **error)
{
	teco_error_qregopunsupported_set(error, qreg->head.name.data, qreg->head.name.len, FALSE);
	return FALSE;
}

static gboolean
teco_qreg_workingdir_undo_append_string(teco_qreg_t *qreg, GError **error)
{
	return TRUE;
}

static gboolean
teco_qreg_workingdir_get_string(teco_qreg_t *qreg, gchar **str, gsize *len, GError **error)
{
	/*
	 * On platforms with a default non-forward-slash directory
	 * separator (i.e. Windows), teco_buffer_t::filename will have
	 * the wrong separator.
	 * To make the life of macros that evaluate "$" easier,
	 * the directory separators are normalized to "/" here.
	 * This does not change the size of the string, so
	 * the return value for str == NULL is still correct.
	 */
	gchar *dir = g_get_current_dir();
	*len = strlen(dir);
	if (str)
		*str = teco_file_normalize_path(dir);
	else
		g_free(dir);

	return TRUE;
}

static gint
teco_qreg_workingdir_get_character(teco_qreg_t *qreg, guint position, GError **error)
{
	g_auto(teco_string_t) str = {NULL, 0};

	if (!teco_qreg_workingdir_get_string(qreg, &str.data, &str.len, error))
		return -1;

	if (position >= str.len) {
		g_set_error(error, TECO_ERROR, TECO_ERROR_RANGE,
		            "Position %u out of range", position);
		return -1;
	}

	return str.data[position];
}

static gboolean
teco_qreg_workingdir_edit(teco_qreg_t *qreg, GError **error)
{
	g_auto(teco_string_t) str = {NULL, 0};

	if (!teco_qreg_plain_edit(qreg, error) ||
	    !teco_qreg_workingdir_get_string(qreg, &str.data, &str.len, error))
		return FALSE;

	teco_view_ssm(teco_qreg_view, SCI_BEGINUNDOACTION, 0, 0);
	teco_view_ssm(teco_qreg_view, SCI_CLEARALL, 0, 0);
	teco_view_ssm(teco_qreg_view, SCI_ADDTEXT, str.len, (sptr_t)str.data);
	teco_view_ssm(teco_qreg_view, SCI_ENDUNDOACTION, 0, 0);

	undo__teco_view_ssm(teco_qreg_view, SCI_UNDO, 0, 0);
	return TRUE;
}

static gboolean
teco_qreg_workingdir_exchange_string(teco_qreg_t *qreg, teco_doc_t *src, GError **error)
{
	g_auto(teco_string_t) other_str, own_str = {NULL, 0};

	teco_doc_get_string(src, &other_str.data, &other_str.len);

	if (!teco_qreg_workingdir_get_string(qreg, &own_str.data, &own_str.len, error) ||
	    /* FIXME: Why is teco_qreg_plain_set_string() sufficient? */
	    !teco_qreg_plain_set_string(qreg, other_str.data, other_str.len, error))
		return FALSE;

	teco_doc_set_string(src, own_str.data, own_str.len);
	return TRUE;
}

static gboolean
teco_qreg_workingdir_undo_exchange_string(teco_qreg_t *qreg, teco_doc_t *src, GError **error)
{
	teco_undo_change_dir_to_current();
	if (qreg->must_undo) // FIXME
		teco_doc_undo_set_string(src);
	return TRUE;
}

/** @static @memberof teco_qreg_t */
teco_qreg_t *
teco_qreg_workingdir_new(void)
{
	static teco_qreg_vtable_t vtable = TECO_INIT_QREG(
		.set_string		= teco_qreg_workingdir_set_string,
		.undo_set_string	= teco_qreg_workingdir_undo_set_string,
		.append_string		= teco_qreg_workingdir_append_string,
		.undo_append_string	= teco_qreg_workingdir_undo_append_string,
		.get_string		= teco_qreg_workingdir_get_string,
		.get_character		= teco_qreg_workingdir_get_character,
		.edit			= teco_qreg_workingdir_edit,
		.exchange_string	= teco_qreg_workingdir_exchange_string,
		.undo_exchange_string	= teco_qreg_workingdir_undo_exchange_string
	);

	/*
	 * FIXME: Dollar is not the best name for it since it is already
	 * heavily overloaded in the language and easily confused with Escape and
	 * the "\e" register also exists.
	 * Not to mention that environment variable regs also start with dollar.
	 * Perhaps "~" would be a better choice, although it is also already used?
	 * Most logical would be ".", but this should probably map to to Dot and
	 * is also ugly to write in practice.
	 * Perhaps "@"...
	 */
	return teco_qreg_new(&vtable, "$", 1);
}

static gboolean
teco_qreg_clipboard_set_string(teco_qreg_t *qreg, const gchar *str, gsize len, GError **error)
{
	g_assert(!teco_string_contains(&qreg->head.name, '\0'));
	const gchar *clipboard_name = qreg->head.name.data + 1;

	if (teco_ed & TECO_ED_AUTOEOL) {
		/*
		 * NOTE: Currently uses GString instead of teco_string_t to make use of
		 * preallocation.
		 * On the other hand GString has a higher overhead.
		 */
		g_autoptr(GString) str_converted = g_string_sized_new(len);

		/*
		 * This will convert to the Q-Register view's EOL mode.
		 */
		g_auto(teco_eol_writer_t) writer;
		teco_eol_writer_init_mem(&writer, teco_view_ssm(teco_qreg_view, SCI_GETEOLMODE, 0, 0),
		                         str_converted);

		gssize bytes_written = teco_eol_writer_convert(&writer, str, len, error);
		if (bytes_written < 0)
			return FALSE;
		g_assert(bytes_written == len);

		return teco_interface_set_clipboard(clipboard_name, str_converted->str,
		                                    str_converted->len, error);
	} else {
		/*
		 * No EOL conversion necessary. The teco_eol_writer_t can handle
		 * this as well, but will result in unnecessary allocations.
		 */
		return teco_interface_set_clipboard(clipboard_name, str, len, error);
	}

	/* should not be reached */
	return TRUE;
}

/*
 * FIXME: Redundant with teco_qreg_bufferinfo_append_string()...
 * Best solution would be to simply implement them.
 */
static gboolean
teco_qreg_clipboard_append_string(teco_qreg_t *qreg, const gchar *str, gsize len, GError **error)
{
	teco_error_qregopunsupported_set(error, qreg->head.name.data, qreg->head.name.len, FALSE);
	return FALSE;
}

static gboolean
teco_qreg_clipboard_undo_append_string(teco_qreg_t *qreg, GError **error)
{
	return TRUE;
}

static gboolean
teco_qreg_clipboard_undo_set_string(teco_qreg_t *qreg, GError **error)
{
	/*
	 * Upon rubout, the current contents of the clipboard are
	 * restored.
	 * We are checking for teco_undo_enabled instead of relying on
	 * teco_undo_push(), since getting the clipboard
	 * is an expensive operation that we want to avoid.
	 */
	if (!teco_undo_enabled)
		return TRUE;

	g_assert(!teco_string_contains(&qreg->head.name, '\0'));
	const gchar *clipboard_name = qreg->head.name.data + 1;

	/*
	 * Ownership of str is passed to the undo token.
	 * This avoids any EOL translation as that would be cumbersome
	 * and could also modify the clipboard in unexpected ways.
	 */
	teco_string_t str;
	if (!teco_interface_get_clipboard(clipboard_name, &str.data, &str.len, error))
		return FALSE;
	teco_interface_undo_set_clipboard(clipboard_name, str.data, str.len);
	return TRUE;
}

static gboolean
teco_qreg_clipboard_get_string(teco_qreg_t *qreg, gchar **str, gsize *len, GError **error)
{
	g_assert(!teco_string_contains(&qreg->head.name, '\0'));
	const gchar *clipboard_name = qreg->head.name.data + 1;

	if (!(teco_ed & TECO_ED_AUTOEOL))
		/*
		 * No auto-eol conversion - avoid unnecessary copying and allocations.
		 */
		return teco_interface_get_clipboard(clipboard_name, str, len, error);

	g_auto(teco_string_t) temp = {NULL, 0};
	if (!teco_interface_get_clipboard(clipboard_name, &temp.data, &temp.len, error))
		return FALSE;

	g_auto(teco_eol_reader_t) reader;
	teco_eol_reader_init_mem(&reader, temp.data, temp.len);

	/*
	 * FIXME: Could be simplified if teco_eol_reader_convert_all() had the
	 * same conventions for passing NULL pointers.
	 */
	teco_string_t str_converted;
	if (teco_eol_reader_convert_all(&reader, &str_converted.data,
	                                &str_converted.len, error) == G_IO_STATUS_ERROR)
		return FALSE;

	if (str)
		*str = str_converted.data;
	else
		teco_string_clear(&str_converted);
	*len = str_converted.len;

	return TRUE;
}

static gint
teco_qreg_clipboard_get_character(teco_qreg_t *qreg, guint position, GError **error)
{
	g_auto(teco_string_t) str = {NULL, 0};

	if (!teco_qreg_clipboard_get_string(qreg, &str.data, &str.len, error))
		return -1;

	if (position >= str.len) {
		g_set_error(error, TECO_ERROR, TECO_ERROR_RANGE,
		            "Position %u out of range", position);
		return -1;
	}

	return str.data[position];
}

static gboolean
teco_qreg_clipboard_edit(teco_qreg_t *qreg, GError **error)
{
	if (!teco_qreg_plain_edit(qreg, error))
		return FALSE;

	g_auto(teco_string_t) str = {NULL, 0};

	if (!teco_qreg_clipboard_get_string(qreg, &str.data, &str.len, error))
		return FALSE;

	teco_view_ssm(teco_qreg_view, SCI_BEGINUNDOACTION, 0, 0);
	teco_view_ssm(teco_qreg_view, SCI_CLEARALL, 0, 0);
	teco_view_ssm(teco_qreg_view, SCI_APPENDTEXT, str.len, (sptr_t)str.data);
	teco_view_ssm(teco_qreg_view, SCI_ENDUNDOACTION, 0, 0);

	undo__teco_view_ssm(teco_qreg_view, SCI_UNDO, 0, 0);
	return TRUE;
}

/*
 * FIXME: Very similar to teco_qreg_workingdir_exchange_string().
 */
static gboolean
teco_qreg_clipboard_exchange_string(teco_qreg_t *qreg, teco_doc_t *src, GError **error)
{
	g_auto(teco_string_t) other_str, own_str = {NULL, 0};

	teco_doc_get_string(src, &other_str.data, &other_str.len);

	if (!teco_qreg_clipboard_get_string(qreg, &own_str.data, &own_str.len, error) ||
	    /* FIXME: Why is teco_qreg_plain_set_string() sufficient? */
	    !teco_qreg_plain_set_string(qreg, other_str.data, other_str.len, error))
		return FALSE;

	teco_doc_set_string(src, own_str.data, own_str.len);
	return TRUE;
}

/*
 * FIXME: Very similar to teco_qreg_workingdir_undo_exchange_string().
 */
static gboolean
teco_qreg_clipboard_undo_exchange_string(teco_qreg_t *qreg, teco_doc_t *src, GError **error)
{
	if (!teco_qreg_clipboard_undo_set_string(qreg, error))
		return FALSE;
	if (qreg->must_undo) // FIXME
		teco_doc_undo_set_string(src);
	return TRUE;
}

/** @static @memberof teco_qreg_t */
teco_qreg_t *
teco_qreg_clipboard_new(const gchar *name)
{
	static teco_qreg_vtable_t vtable = TECO_INIT_QREG(
		.set_string		= teco_qreg_clipboard_set_string,
		.undo_set_string	= teco_qreg_clipboard_undo_set_string,
		.append_string		= teco_qreg_clipboard_append_string,
		.undo_append_string	= teco_qreg_clipboard_undo_append_string,
		.get_string		= teco_qreg_clipboard_get_string,
		.get_character		= teco_qreg_clipboard_get_character,
		.edit			= teco_qreg_clipboard_edit,
		.exchange_string	= teco_qreg_clipboard_exchange_string,
		.undo_exchange_string	= teco_qreg_clipboard_undo_exchange_string
	);

	teco_qreg_t *qreg = teco_qreg_new(&vtable, "~", 1);
	teco_string_append(&qreg->head.name, name, strlen(name));
	return qreg;
}

/** @memberof teco_qreg_table_t */
void
teco_qreg_table_init(teco_qreg_table_t *table, gboolean must_undo)
{
	rb3_reset_tree(&table->tree);
	table->must_undo = must_undo;

	/* general purpose registers */
	for (gchar q = 'A'; q <= 'Z'; q++)
		teco_qreg_table_insert(table, teco_qreg_plain_new(&q, sizeof(q)));
	for (gchar q = '0'; q <= '9'; q++)
		teco_qreg_table_insert(table, teco_qreg_plain_new(&q, sizeof(q)));
}

static inline void
teco_qreg_table_remove(teco_qreg_t *reg)
{
	rb3_unlink_and_rebalance(&reg->head.head);
	teco_qreg_free(reg);
}
TECO_DEFINE_UNDO_CALL(teco_qreg_table_remove, teco_qreg_t *);

static inline void
teco_qreg_table_undo_remove(teco_qreg_t *qreg)
{
	if (qreg->must_undo)
		undo__teco_qreg_table_remove(qreg);
}

/** @memberof teco_qreg_table_t */
teco_qreg_t *
teco_qreg_table_edit_name(teco_qreg_table_t *table, const gchar *name, gsize len, GError **error)
{
	teco_qreg_t *qreg = teco_qreg_table_find(table, name, len);
	if (!qreg) {
		g_autofree gchar *name_printable = teco_string_echo(name, len);
		g_set_error(error, TECO_ERROR, TECO_ERROR_FAILED,
		            "Q-Register \"%s\" not found", name_printable);
		return NULL;
	}

	return teco_qreg_table_edit(table, qreg, error) ? qreg : NULL;
}

/**
 * Import process environment into table
 * by setting environment registers for every
 * environment variable.
 * It is assumed that the table does not yet
 * contain any environment register.
 *
 * In general this method is only safe to call
 * at startup.
 *
 * @memberof teco_qreg_table_t
 */
gboolean
teco_qreg_table_set_environ(teco_qreg_table_t *table, GError **error)
{
	/*
	 * NOTE: Using g_get_environ() would be more efficient,
	 * but it appears to be broken, at least on Windows 2000.
	 */
	g_auto(GStrv) env = g_listenv();

	for (gchar **key = env; *key; key++) {
		g_autofree gchar *name = g_strconcat("$", *key, NULL);

		/*
		 * FIXME: It might be a good idea to wrap this into
		 * a convenience function.
		 */
		teco_qreg_t *qreg = teco_qreg_plain_new(name, strlen(name));
		teco_qreg_t *found = teco_qreg_table_insert(table, qreg);
		if (found) {
			teco_qreg_free(qreg);
			qreg = found;
		}

		const gchar *value = g_getenv(*key);
		g_assert(value != NULL);
		if (!qreg->vtable->set_string(qreg, value, strlen(value), error))
			return FALSE;
	}

	return TRUE;
}

/**
 * Export environment registers as a list of environment
 * variables compatible with `g_get_environ()`.
 *
 * @return Zero-terminated list of strings in the form
 *         `NAME=VALUE`. Should be freed with `g_strfreev()`.
 *         NULL in case of errors.
 *
 * @memberof teco_qreg_table_t
 */
gchar **
teco_qreg_table_get_environ(teco_qreg_table_t *table, GError **error)
{
	teco_qreg_t *first = (teco_qreg_t *)teco_rb3str_nfind(&table->tree, TRUE, "$", 1);

	gint envp_len = 1;

	/*
	 * Iterate over all registers beginning with "$" to
	 * guess the size required for the environment array.
	 * This may waste a few bytes because not __every__
	 * register beginning with "$" is an environment
	 * register.
	 */
	for (teco_qreg_t *cur = first;
	     cur && cur->head.name.data[0] == '$';
	     cur = (teco_qreg_t *)teco_rb3str_get_next(&cur->head))
		envp_len++;

	gchar **envp, **p;
	p = envp = g_new(gchar *, envp_len);

	for (teco_qreg_t *cur = first;
	     cur && cur->head.name.data[0] == '$';
	     cur = (teco_qreg_t *)teco_rb3str_get_next(&cur->head)) {
		const teco_string_t *name = &cur->head.name;

		/*
		 * Ignore the "$" register (not an environment
		 * variable register) and registers whose
		 * name contains "=" or null (not allowed in environment
		 * variable names).
		 */
		if (name->len == 1 ||
		    teco_string_contains(name, '=') || teco_string_contains(name, '\0'))
			continue;

		g_auto(teco_string_t) value = {NULL, 0};
		if (!cur->vtable->get_string(cur, &value.data, &value.len, error)) {
			g_strfreev(envp);
			return NULL;
		}
		if (teco_string_contains(&value, '\0')) {
			g_strfreev(envp);
			g_set_error(error, TECO_ERROR, TECO_ERROR_FAILED,
			            "Environment register \"%s\" must not contain null characters",
			            name->data);
			return NULL;
		}

		/* more efficient than g_environ_setenv() */
		*p++ = g_strconcat(name->data+1, "=", value.data, NULL);
	}

	*p = NULL;

	return envp;
}

/**
 * Empty Q-Register table except the currently edited register.
 * If the table contains the currently edited register, it will
 * throw an error and the table might be left half-emptied.
 *
 * @memberof teco_qreg_table_t
 */
gboolean
teco_qreg_table_empty(teco_qreg_table_t *table, GError **error)
{
	struct rb3_head *cur;

	while ((cur = rb3_get_root(&table->tree))) {
		if ((teco_qreg_t *)cur == teco_qreg_current) {
			const teco_string_t *name = &teco_qreg_current->head.name;
			g_autofree gchar *name_printable = teco_string_echo(name->data, name->len);
			g_set_error(error, TECO_ERROR, TECO_ERROR_FAILED,
			            "Currently edited Q-Register \"%s\" cannot be discarded", name_printable);
			return FALSE;
		}

		rb3_unlink_and_rebalance(cur);
		teco_qreg_free((teco_qreg_t *)cur);
	}

	return TRUE;
}

/** @memberof teco_qreg_table_t */
void
teco_qreg_table_clear(teco_qreg_table_t *table)
{
	struct rb3_head *cur;

	while ((cur = rb3_get_root(&table->tree))) {
		rb3_unlink_and_rebalance(cur);
		teco_qreg_free((teco_qreg_t *)cur);
	}
}

typedef struct {
	teco_int_t integer;
	teco_doc_t string;
} teco_qreg_stack_entry_t;

static inline void
teco_qreg_stack_entry_clear(teco_qreg_stack_entry_t *entry)
{
	teco_doc_clear(&entry->string);
}

static GArray *teco_qreg_stack;

static void __attribute__((constructor))
teco_qreg_stack_init(void)
{
	teco_qreg_stack = g_array_sized_new(FALSE, FALSE, sizeof(teco_qreg_stack_entry_t), 1024);
}

static inline void
teco_qreg_stack_remove_last(void)
{
	teco_qreg_stack_entry_clear(&g_array_index(teco_qreg_stack, teco_qreg_stack_entry_t,
	                                           teco_qreg_stack->len-1));
	g_array_remove_index(teco_qreg_stack, teco_qreg_stack->len-1);
}
TECO_DEFINE_UNDO_CALL(teco_qreg_stack_remove_last);

gboolean
teco_qreg_stack_push(teco_qreg_t *qreg, GError **error)
{
	teco_qreg_stack_entry_t entry;
	g_auto(teco_string_t) string = {NULL, 0};

	if (!qreg->vtable->get_integer(qreg, &entry.integer, error) ||
	    !qreg->vtable->get_string(qreg, &string.data, &string.len, error))
		return FALSE;
	teco_doc_init(&entry.string);
	teco_doc_set_string(&entry.string, string.data, string.len);
	teco_doc_update(&entry.string, &qreg->string);

	/* pass ownership of entry to teco_qreg_stack */
	g_array_append_val(teco_qreg_stack, entry);
	undo__teco_qreg_stack_remove_last();
	return TRUE;
}

static void
teco_qreg_stack_entry_action(teco_qreg_stack_entry_t *entry, gboolean run)
{
	if (run)
		g_array_append_val(teco_qreg_stack, *entry);
	else
		teco_qreg_stack_entry_clear(entry);
}

static void
teco_undo_qreg_stack_push_own(teco_qreg_stack_entry_t *entry)
{
	teco_qreg_stack_entry_t *ctx = teco_undo_push(teco_qreg_stack_entry);
	if (ctx)
		memcpy(ctx, entry, sizeof(*ctx));
	else
		teco_qreg_stack_entry_clear(entry);
}

gboolean
teco_qreg_stack_pop(teco_qreg_t *qreg, GError **error)
{
	if (!teco_qreg_stack->len) {
		g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
		                    "Q-Register stack empty");
		return FALSE;
	}

	teco_qreg_stack_entry_t *entry;
	entry = &g_array_index(teco_qreg_stack, teco_qreg_stack_entry_t, teco_qreg_stack->len-1);

	if (!qreg->vtable->undo_set_integer(qreg, error) ||
	    !qreg->vtable->set_integer(qreg, entry->integer, error))
		return FALSE;

	/* exchange document ownership between stack entry and Q-Register */
	if (!qreg->vtable->undo_exchange_string(qreg, &entry->string, error) ||
	    !qreg->vtable->exchange_string(qreg, &entry->string, error))
		return FALSE;

	/* pass entry ownership to undo stack. */
	teco_undo_qreg_stack_push_own(entry);

	g_array_remove_index(teco_qreg_stack, teco_qreg_stack->len-1);
	return TRUE;
}

void
teco_qreg_stack_clear(void)
{
	g_array_set_clear_func(teco_qreg_stack, (GDestroyNotify)teco_qreg_stack_entry_clear);
	g_array_free(teco_qreg_stack, TRUE);
}

gboolean
teco_ed_hook(teco_ed_hook_t type, GError **error)
{
	if (!(teco_ed & TECO_ED_HOOKS))
		return TRUE;

	/*
	 * NOTE: It is crucial to declare this before the first goto,
	 * since it runs all destructors.
	 */
	g_auto(teco_qreg_table_t) locals;
	teco_qreg_table_init(&locals, FALSE);

	teco_qreg_t *qreg = teco_qreg_table_find(&teco_qreg_table_globals, "ED", 2);
	if (!qreg) {
		g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
		                    "Undefined ED-hook register (\"ED\")");
		goto error_add_frame;
	}

	/*
	 * ED-hook execution should not see any
	 * integer parameters but the hook type.
	 * Such parameters could confuse the ED macro
	 * and macro authors do not expect side effects
	 * of ED macros on the expression stack.
	 * Also make sure it does not leave behind
	 * additional arguments on the stack.
	 *
	 * So this effectively executes:
	 * (typeM[ED]^[)
	 *
	 * FIXME: Temporarily stashing away the expression
	 * stack may be a more elegant solution.
	 */
	teco_expressions_brace_open();
	teco_expressions_push_int(type);

	if (!teco_qreg_execute(qreg, &locals, error))
		goto error_add_frame;

	return teco_expressions_discard_args(error) &&
	       teco_expressions_brace_close(error);

	static const gchar *type2name[] = {
		[TECO_ED_HOOK_ADD-1]   = "ADD",
		[TECO_ED_HOOK_EDIT-1]  = "EDIT",
		[TECO_ED_HOOK_CLOSE-1] = "CLOSE",
		[TECO_ED_HOOK_QUIT-1]  = "QUIT"
	};

error_add_frame:
	g_assert(0 <= type-1 && type-1 < G_N_ELEMENTS(type2name));
	teco_error_add_frame_edhook(type2name[type-1]);
	return FALSE;
}

/** @extends teco_machine_t */
struct teco_machine_qregspec_t {
	teco_machine_t parent;

	/**
	 * Aliases bitfield with an integer.
	 * This allows teco_undo_guint(__flags),
	 * while still supporting easy-to-access flags.
	 */
	union {
		struct {
			teco_qreg_type_t type : 8;
			gboolean parse_only : 1;
		};
		guint __flags;
	};

	/** Local Q-Register table of the macro invocation frame. */
	teco_qreg_table_t *qreg_table_locals;

	teco_machine_stringbuilding_t machine_stringbuilding;
	/*
	 * FIXME: Does it make sense to allow nested braces?
	 * Perhaps it's sufficient to support ^Q].
	 */
	gint nesting;
	teco_string_t name;

	teco_qreg_t *result;
	teco_qreg_table_t *result_table;
};

/*
 * FIXME: All teco_state_qregspec_* states could be static?
 */
TECO_DECLARE_STATE(teco_state_qregspec_start);
TECO_DECLARE_STATE(teco_state_qregspec_start_global);
TECO_DECLARE_STATE(teco_state_qregspec_firstchar);
TECO_DECLARE_STATE(teco_state_qregspec_secondchar);
TECO_DECLARE_STATE(teco_state_qregspec_string);

static teco_state_t *teco_state_qregspec_start_global_input(teco_machine_qregspec_t *ctx,
                                                            gchar chr, GError **error);

static teco_state_t *
teco_state_qregspec_done(teco_machine_qregspec_t *ctx, GError **error)
{
	if (ctx->parse_only)
		return &teco_state_qregspec_start;

	ctx->result = teco_qreg_table_find(ctx->result_table, ctx->name.data, ctx->name.len);

	switch (ctx->type) {
	case TECO_QREG_REQUIRED:
		if (!ctx->result) {
			teco_error_invalidqreg_set(error, ctx->name.data, ctx->name.len,
			                           ctx->result_table != &teco_qreg_table_globals);
			return NULL;
		}
		break;

	case TECO_QREG_OPTIONAL:
		break;

	case TECO_QREG_OPTIONAL_INIT:
		if (!ctx->result) {
			ctx->result = teco_qreg_plain_new(ctx->name.data, ctx->name.len);
			teco_qreg_table_insert(ctx->result_table, ctx->result);
			teco_qreg_table_undo_remove(ctx->result);
		}
		break;
	}

	return &teco_state_qregspec_start;
}

static teco_state_t *
teco_state_qregspec_start_input(teco_machine_qregspec_t *ctx, gchar chr, GError **error)
{
	/*
	 * FIXME: We're using teco_state_qregspec_start as a success condition,
	 * so either '.' goes into its own state or we re-introduce a state attribute.
	 */
	if (chr == '.') {
		if (ctx->parent.must_undo)
			teco_undo_ptr(ctx->result_table);
		ctx->result_table = ctx->qreg_table_locals;
		return &teco_state_qregspec_start_global;
	}

	return teco_state_qregspec_start_global_input(ctx, chr, error);
}

/* in cmdline.c */
gboolean teco_state_qregspec_process_edit_cmd(teco_machine_qregspec_t *ctx, teco_machine_t *parent_ctx, gchar key, GError **error);

TECO_DEFINE_STATE(teco_state_qregspec_start,
	.is_start = TRUE,
	.process_edit_cmd_cb = (teco_state_process_edit_cmd_cb_t)teco_state_qregspec_process_edit_cmd
);

static teco_state_t *
teco_state_qregspec_start_global_input(teco_machine_qregspec_t *ctx, gchar chr, GError **error)
{
	/*
	 * FIXME: Disallow space characters?
	 */
	switch (chr) {
	case '#':
		return &teco_state_qregspec_firstchar;

	case '[':
		if (ctx->parent.must_undo)
			teco_undo_gint(ctx->nesting);
		ctx->nesting++;
		return &teco_state_qregspec_string;
	}

	if (!ctx->parse_only) {
		if (ctx->parent.must_undo)
			undo__teco_string_truncate(&ctx->name, ctx->name.len);
		teco_string_append_c(&ctx->name, g_ascii_toupper(chr));
	}
	return teco_state_qregspec_done(ctx, error);
}

/*
 * NOTE: This state mainly exists so that we don't have to go back to teco_state_qregspec_start after
 * an initial `.` -- this is currently used in teco_machine_qregspec_input() to check for completeness.
 * Alternatively, we'd have to introduce a teco_machine_qregspec_t::status attribute.
 * Or even better, why not use special pointers like ((teco_state_t *)"teco_state_qregspec_done")?
 */
TECO_DEFINE_STATE(teco_state_qregspec_start_global,
	.process_edit_cmd_cb = (teco_state_process_edit_cmd_cb_t)teco_state_qregspec_process_edit_cmd
);

static teco_state_t *
teco_state_qregspec_firstchar_input(teco_machine_qregspec_t *ctx, gchar chr, GError **error)
{
	/*
	 * FIXME: Disallow space characters?
	 */
	if (!ctx->parse_only) {
		if (ctx->parent.must_undo)
			undo__teco_string_truncate(&ctx->name, ctx->name.len);
		teco_string_append_c(&ctx->name, g_ascii_toupper(chr));
	}
	return &teco_state_qregspec_secondchar;
}

TECO_DEFINE_STATE(teco_state_qregspec_firstchar,
	.process_edit_cmd_cb = (teco_state_process_edit_cmd_cb_t)teco_state_qregspec_process_edit_cmd
);

static teco_state_t *
teco_state_qregspec_secondchar_input(teco_machine_qregspec_t *ctx, gchar chr, GError **error)
{
	/*
	 * FIXME: Disallow space characters?
	 */
	if (!ctx->parse_only) {
		if (ctx->parent.must_undo)
			undo__teco_string_truncate(&ctx->name, ctx->name.len);
		teco_string_append_c(&ctx->name, g_ascii_toupper(chr));
	}
	return teco_state_qregspec_done(ctx, error);
}

TECO_DEFINE_STATE(teco_state_qregspec_secondchar,
	.process_edit_cmd_cb = (teco_state_process_edit_cmd_cb_t)teco_state_qregspec_process_edit_cmd
);

static teco_state_t *
teco_state_qregspec_string_input(teco_machine_qregspec_t *ctx, gchar chr, GError **error)
{
	/*
	 * Makes sure that braces within string building constructs do not have to be
	 * escaped and that ^Q/^R can be used to escape braces.
	 *
	 * FIXME: Perhaps that's sufficient and we don't have to keep track of nesting?
	 */
	if (ctx->machine_stringbuilding.parent.current->is_start) {
		switch (chr) {
		case '[':
			if (ctx->parent.must_undo)
				teco_undo_gint(ctx->nesting);
			ctx->nesting++;
			break;
		case ']':
			if (ctx->parent.must_undo)
				teco_undo_gint(ctx->nesting);
			ctx->nesting--;
			if (!ctx->nesting)
				return teco_state_qregspec_done(ctx, error);
			break;
		}
	}

	if (!ctx->parse_only && ctx->parent.must_undo)
		undo__teco_string_truncate(&ctx->name, ctx->name.len);

	/*
	 * NOTE: machine_stringbuilding gets notified about parse-only mode by passing NULL
	 * as the target string.
	 */
	if (!teco_machine_stringbuilding_input(&ctx->machine_stringbuilding, chr,
	                                       ctx->parse_only ? NULL : &ctx->name, error))
		return NULL;

	return &teco_state_qregspec_string;
}

/* in cmdline.c */
gboolean teco_state_qregspec_string_process_edit_cmd(teco_machine_qregspec_t *ctx, teco_machine_t *parent_ctx,
                                                     gchar key, GError **error);

TECO_DEFINE_STATE(teco_state_qregspec_string,
	.process_edit_cmd_cb = (teco_state_process_edit_cmd_cb_t)teco_state_qregspec_string_process_edit_cmd
);

/** @static @memberof teco_machine_qregspec_t */
teco_machine_qregspec_t *
teco_machine_qregspec_new(teco_qreg_type_t type, teco_qreg_table_t *locals, gboolean must_undo)
{
	/*
	 * FIXME: Allocate via g_slice?
	 */
	teco_machine_qregspec_t *ctx = g_new0(teco_machine_qregspec_t, 1);
	teco_machine_init(&ctx->parent, &teco_state_qregspec_start, must_undo);
	ctx->type = type;
	ctx->qreg_table_locals = locals;
	teco_machine_stringbuilding_init(&ctx->machine_stringbuilding, '[', locals, must_undo);
	ctx->result_table = &teco_qreg_table_globals;
	return ctx;
}

/** @memberof teco_machine_qregspec_t */
void
teco_machine_qregspec_reset(teco_machine_qregspec_t *ctx)
{
	teco_machine_reset(&ctx->parent, &teco_state_qregspec_start);
	teco_machine_stringbuilding_reset(&ctx->machine_stringbuilding);
	if (ctx->parent.must_undo) {
		teco_undo_string_own(ctx->name);
		teco_undo_gint(ctx->nesting);
		teco_undo_guint(ctx->__flags);
	} else {
		teco_string_clear(&ctx->name);
	}
	memset(&ctx->name, 0, sizeof(ctx->name));
	ctx->nesting = 0;
	ctx->result_table = &teco_qreg_table_globals;
}

/** @memberof teco_machine_qregspec_t */
teco_machine_stringbuilding_t *
teco_machine_qregspec_get_stringbuilding(teco_machine_qregspec_t *ctx)
{
	return &ctx->machine_stringbuilding;
}

/**
 * Pass a character to the QRegister specification machine.
 *
 * @param ctx QRegister specification machine.
 * @param chr Character to parse.
 * @param result Pointer to QRegister or NULL in parse-only mode.
 *               If non-NULL it will be set once a specification is successfully parsed.
 * @param result_table Pointer to QRegister table. May be NULL in parse-only mode.
 * @param error GError or NULL.
 * @return Returns TECO_MACHINE_QREGSPEC_DONE in case of complete specs.
 *
 * @memberof teco_machine_qregspec_t
 */
teco_machine_qregspec_status_t
teco_machine_qregspec_input(teco_machine_qregspec_t *ctx, gchar chr,
                            teco_qreg_t **result, teco_qreg_table_t **result_table, GError **error)
{
	ctx->parse_only = result == NULL;

	if (!teco_machine_input(&ctx->parent, chr, error))
		return TECO_MACHINE_QREGSPEC_ERROR;

	teco_machine_qregspec_get_results(ctx, result, result_table);
	return ctx->parent.current == &teco_state_qregspec_start
			? TECO_MACHINE_QREGSPEC_DONE : TECO_MACHINE_QREGSPEC_MORE;
}

/** @memberof teco_machine_qregspec_t */
void
teco_machine_qregspec_get_results(teco_machine_qregspec_t *ctx,
                                  teco_qreg_t **result, teco_qreg_table_t **result_table)
{
	if (result)
		*result = ctx->result;
	if (result_table)
		*result_table = ctx->result_table;
}

/** @memberof teco_machine_qregspec_t */
gboolean
teco_machine_qregspec_auto_complete(teco_machine_qregspec_t *ctx, teco_string_t *insert)
{
	gsize restrict_len = 0;

	/*
	 * NOTE: We could have separate process_edit_cmd_cb() for
	 * teco_state_qregspec_firstchar/teco_state_qregspec_secondchar
	 * and pass down restrict_len instead.
	 */
	if (ctx->parent.current == &teco_state_qregspec_start ||
	    ctx->parent.current == &teco_state_qregspec_start_global)
		/* single-letter Q-Reg */
		restrict_len = 1;
	else if (ctx->parent.current != &teco_state_qregspec_string)
		/* two-letter Q-Reg */
		restrict_len = 2;

	return teco_rb3str_auto_complete(&ctx->result_table->tree, !restrict_len,
	                                 ctx->name.data, ctx->name.len, restrict_len, insert) &&
	       ctx->nesting == 1;
}

/** @memberof teco_machine_qregspec_t */
void
teco_machine_qregspec_free(teco_machine_qregspec_t *ctx)
{
	teco_machine_stringbuilding_clear(&ctx->machine_stringbuilding);
	teco_string_clear(&ctx->name);
	g_free(ctx);
}

TECO_DEFINE_UNDO_CALL(teco_machine_qregspec_free, teco_machine_qregspec_t *);
TECO_DEFINE_UNDO_OBJECT_OWN(qregspec, teco_machine_qregspec_t *, teco_machine_qregspec_free);
