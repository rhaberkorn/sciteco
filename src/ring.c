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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib.h>
#include <glib/gprintf.h>

#include <Scintilla.h>

#include "sciteco.h"
#include "file-utils.h"
#include "interface.h"
#include "view.h"
#include "undo.h"
#include "parser.h"
#include "core-commands.h"
#include "expressions.h"
#include "qreg.h"
#include "glob.h"
#include "error.h"
#include "list.h"
#include "ring.h"

/** @private @static @memberof teco_buffer_t */
static teco_buffer_t *
teco_buffer_new(void)
{
	teco_buffer_t *ctx = g_new0(teco_buffer_t, 1);
	ctx->view = teco_view_new();
	teco_view_setup(ctx->view);
	return ctx;
}

/** @private @memberof teco_buffer_t */
static void
teco_buffer_set_filename(teco_buffer_t *ctx, const gchar *filename)
{
	gchar *resolved = teco_file_get_absolute_path(filename);
	g_free(ctx->filename);
	ctx->filename = resolved;
	teco_interface_info_update(ctx);
}

/** @memberof teco_buffer_t */
void
teco_buffer_edit(teco_buffer_t *ctx)
{
	teco_interface_show_view(ctx->view);
	teco_interface_info_update(ctx);
}
/** @memberof teco_buffer_t */
void
teco_buffer_undo_edit(teco_buffer_t *ctx)
{
	undo__teco_interface_info_update_buffer(ctx);
	undo__teco_interface_show_view(ctx->view);
}

/** @private @memberof teco_buffer_t */
static gboolean
teco_buffer_load(teco_buffer_t *ctx, const gchar *filename, GError **error)
{
	if (!teco_view_load(ctx->view, filename, error))
		return FALSE;

#if 0		/* NOTE: currently buffer cannot be dirty */
	undo__teco_interface_info_update_buffer(ctx);
	teco_undo_gboolean(ctx->dirty) = FALSE;
#endif

	teco_buffer_set_filename(ctx, filename);
	return TRUE;
}

/** @private @memberof teco_buffer_t */
static gboolean
teco_buffer_save(teco_buffer_t *ctx, const gchar *filename, GError **error)
{
	if (!filename && !ctx->filename) {
		g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
		                    "Cannot save the unnamed file "
		                    "without providing a file name");
		return FALSE;
	}

	if (!teco_view_save(ctx->view, filename ? : ctx->filename, error))
		return FALSE;

	/*
	 * Undirtify
	 * NOTE: info update is performed by set_filename()
	 */
	undo__teco_interface_info_update_buffer(ctx);
	teco_undo_gboolean(ctx->dirty) = FALSE;

	/*
	 * FIXME: necessary also if the filename was not specified but the file
	 * is (was) new, in order to canonicalize the filename.
	 * May be circumvented by cananonicalizing without requiring the file
	 * name to exist (like readlink -f)
	 * NOTE: undo_info_update is already called above
	 */
	teco_undo_cstring(ctx->filename);
	teco_buffer_set_filename(ctx, filename ? : ctx->filename);

	return TRUE;
}

/** @private @memberof teco_buffer_t */
static inline void
teco_buffer_free(teco_buffer_t *ctx)
{
	teco_view_free(ctx->view);
	g_free(ctx->filename);
	g_free(ctx);
}

TECO_DEFINE_UNDO_CALL(teco_buffer_free, teco_buffer_t *);

static teco_tailq_entry_t teco_ring_head = TECO_TAILQ_HEAD_INITIALIZER(&teco_ring_head);

teco_buffer_t *teco_ring_current = NULL;

teco_buffer_t *
teco_ring_first(void)
{
	return (teco_buffer_t *)teco_ring_head.first;
}

teco_buffer_t *
teco_ring_last(void)
{
	return (teco_buffer_t *)teco_ring_head.last->prev->next;
}

static void
teco_undo_ring_edit_action(teco_buffer_t **buffer, gboolean run)
{
	if (run) {
		/*
		 * assumes that buffer still has correct prev/next
		 * pointers
		 */
		if (teco_buffer_next(*buffer))
			teco_tailq_insert_before((*buffer)->entry.next, &(*buffer)->entry);
		else
			teco_tailq_insert_tail(&teco_ring_head, &(*buffer)->entry);

		teco_ring_current = *buffer;
		teco_buffer_edit(*buffer);
	} else {
		teco_buffer_free(*buffer);
	}
}

/*
 * Emitted after a buffer close
 * The pointer is the only remaining reference to the buffer!
 */
static void
teco_undo_ring_edit(teco_buffer_t *buffer)
{
	teco_buffer_t **ctx = teco_undo_push_size((teco_undo_action_t)teco_undo_ring_edit_action,
	                                          sizeof(buffer));
	if (ctx)
		*ctx = buffer;
	else
		teco_buffer_free(buffer);
}

teco_int_t
teco_ring_get_id(teco_buffer_t *buffer)
{
	teco_int_t ret = 1;

	for (teco_tailq_entry_t *cur = teco_ring_head.first;
	     cur != &buffer->entry;
	     cur = cur->next)
		ret++;

	return ret;
}

teco_buffer_t *
teco_ring_find_by_name(const gchar *filename)
{
	g_autofree gchar *resolved = teco_file_get_absolute_path(filename);

	for (teco_tailq_entry_t *cur = teco_ring_head.first; cur != NULL; cur = cur->next) {
		teco_buffer_t *buffer = (teco_buffer_t *)cur;
		if (!g_strcmp0(buffer->filename, resolved))
			return buffer;
	}

	return NULL;
}

teco_buffer_t *
teco_ring_find_by_id(teco_int_t id)
{
	for (teco_tailq_entry_t *cur = teco_ring_head.first; cur != NULL; cur = cur->next) {
		if (!--id)
			return (teco_buffer_t *)cur;
	}

	return NULL;
}

void
teco_ring_dirtify(void)
{
	if (teco_qreg_current || teco_ring_current->dirty)
		return;

	undo__teco_interface_info_update_buffer(teco_ring_current);
	teco_undo_gboolean(teco_ring_current->dirty) = TRUE;
	teco_interface_info_update(teco_ring_current);
}

gboolean
teco_ring_is_any_dirty(void)
{
	for (teco_tailq_entry_t *cur = teco_ring_head.first; cur != NULL; cur = cur->next) {
		teco_buffer_t *buffer = (teco_buffer_t *)cur;
		if (buffer->dirty)
			return TRUE;
	}

	return FALSE;
}

gboolean
teco_ring_save_all_dirty_buffers(GError **error)
{
	for (teco_tailq_entry_t *cur = teco_ring_head.first; cur != NULL; cur = cur->next) {
		teco_buffer_t *buffer = (teco_buffer_t *)cur;
		/* NOTE: Will fail for a dirty unnamed file */
		if (buffer->dirty && !teco_buffer_save(buffer, NULL, error))
			return FALSE;
	}

	return TRUE;
}

gboolean
teco_ring_edit_by_name(const gchar *filename, GError **error)
{
	teco_buffer_t *buffer = teco_ring_find(filename);

	teco_qreg_current = NULL;
	if (buffer) {
		teco_ring_current = buffer;
		teco_buffer_edit(buffer);

		return teco_ed_hook(TECO_ED_HOOK_EDIT, error);
	}

	buffer = teco_buffer_new();
	teco_tailq_insert_tail(&teco_ring_head, &buffer->entry);

	teco_ring_current = buffer;
	teco_ring_undo_close();

	teco_buffer_edit(buffer);
	if (filename && g_file_test(filename, G_FILE_TEST_IS_REGULAR)) {
		if (!teco_buffer_load(buffer, filename, error))
			return FALSE;

		teco_interface_msg(TECO_MSG_INFO,
		                   "Added file \"%s\" to ring", filename);
	} else {
		teco_buffer_set_filename(buffer, filename);

		if (filename)
			teco_interface_msg(TECO_MSG_INFO,
			                   "Added new file \"%s\" to ring",
			                   filename);
		else
			teco_interface_msg(TECO_MSG_INFO,
			                   "Added new unnamed file to ring.");
	}

	return teco_ed_hook(TECO_ED_HOOK_ADD, error);
}

gboolean
teco_ring_edit_by_id(teco_int_t id, GError **error)
{
	teco_buffer_t *buffer = teco_ring_find(id);
	if (!buffer) {
		g_set_error(error, TECO_ERROR, TECO_ERROR_FAILED,
		            "Invalid buffer id %" TECO_INT_FORMAT, id);
		return FALSE;
	}

	teco_qreg_current = NULL;
	teco_ring_current = buffer;
	teco_buffer_edit(buffer);

	return teco_ed_hook(TECO_ED_HOOK_EDIT, error);
}

static void
teco_ring_close_buffer(teco_buffer_t *buffer)
{
	teco_tailq_remove(&teco_ring_head, &buffer->entry);

	if (buffer->filename)
		teco_interface_msg(TECO_MSG_INFO,
		                   "Removed file \"%s\" from the ring",
		                   buffer->filename);
	else
		teco_interface_msg(TECO_MSG_INFO,
		                   "Removed unnamed file from the ring.");
}

TECO_DEFINE_UNDO_CALL(teco_ring_close_buffer, teco_buffer_t *);

gboolean
teco_ring_close(GError **error)
{
	teco_buffer_t *buffer = teco_ring_current;

	if (!teco_ed_hook(TECO_ED_HOOK_CLOSE, error))
		return FALSE;
	teco_ring_close_buffer(buffer);
	teco_ring_current = teco_buffer_next(buffer) ? : teco_buffer_prev(buffer);
	/* Transfer responsibility to the undo token object. */
	teco_undo_ring_edit(buffer);

	if (!teco_ring_current)
		return teco_ring_edit_by_name(NULL, error);

	teco_buffer_edit(teco_ring_current);
	return teco_ed_hook(TECO_ED_HOOK_EDIT, error);
}

void
teco_ring_undo_close(void)
{
	undo__teco_buffer_free(teco_ring_current);
	undo__teco_ring_close_buffer(teco_ring_current);
}

void
teco_ring_set_scintilla_undo(gboolean state)
{
	for (teco_tailq_entry_t *cur = teco_ring_head.first; cur != NULL; cur = cur->next) {
		teco_buffer_t *buffer = (teco_buffer_t *)cur;
		teco_view_set_scintilla_undo(buffer->view, state);
	}
}

void
teco_ring_cleanup(void)
{
	teco_tailq_entry_t *next;

	for (teco_tailq_entry_t *cur = teco_ring_head.first; cur != NULL; cur = next) {
		next = cur->next;
		teco_buffer_free((teco_buffer_t *)cur);
	}

	teco_ring_head = TECO_TAILQ_HEAD_INITIALIZER(&teco_ring_head);
}

/*
 * Command states
 */

/*
 * FIXME: Should be part of the teco_machine_main_t?
 * Unfortunately, we cannot just merge initial() with done(),
 * since we want to react immediately to xEB without waiting for $.
 */
static gboolean allow_filename = FALSE;

static gboolean
teco_state_edit_file_initial(teco_machine_main_t *ctx, GError **error)
{
	if (ctx->mode > TECO_MODE_NORMAL)
		return TRUE;

	teco_int_t id;
	if (!teco_expressions_pop_num_calc(&id, -1, error))
		return FALSE;

	allow_filename = TRUE;

	if (id == 0) {
		for (teco_buffer_t *cur = teco_ring_first(); cur; cur = teco_buffer_next(cur)) {
			const gchar *filename = cur->filename ? : "(Unnamed)";
			teco_interface_popup_add(TECO_POPUP_FILE, filename,
			                         strlen(filename), cur == teco_ring_current);
		}

		teco_interface_popup_show();
	} else if (id > 0) {
		allow_filename = FALSE;
		if (!teco_current_doc_undo_edit(error) ||
		    !teco_ring_edit(id, error))
			return FALSE;
	}

	return TRUE;
}

static teco_state_t *
teco_state_edit_file_done(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	if (ctx->mode > TECO_MODE_NORMAL)
		return &teco_state_start;

	if (!allow_filename) {
		if (str->len > 0) {
			g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
			                    "If a buffer is selected by id, the <EB> "
			                    "string argument must be empty");
			return NULL;
		}

		return &teco_state_start;
	}

	if (!teco_current_doc_undo_edit(error))
		return NULL;

	g_autofree gchar *filename = teco_file_expand_path(str->data);
	if (teco_globber_is_pattern(filename)) {
		g_auto(teco_globber_t) globber;
		teco_globber_init(&globber, filename, G_FILE_TEST_IS_REGULAR);

		gchar *globbed_filename;
		while ((globbed_filename = teco_globber_next(&globber))) {
			gboolean rc = teco_ring_edit(globbed_filename, error);
			g_free(globbed_filename);
			if (!rc)
				return NULL;
		}
	} else {
		if (!teco_ring_edit_by_name(*filename ? filename : NULL, error))
			return NULL;
	}

	return &teco_state_start;
}

/*$ EB edit
 * [n]EB[file]$ -- Open or edit file
 * nEB$
 *
 * Opens or edits the file with name <file>.
 * If <file> is not in the buffer ring it is opened,
 * added to the ring and set as the currently edited
 * buffer.
 * If it already exists in the ring, it is merely
 * made the current file.
 * <file> may be omitted in which case the default
 * unnamed buffer is created/edited.
 * If an argument is specified as 0, EB will additionally
 * display the buffer ring contents in the window's popup
 * area.
 * Naturally this only has any effect in interactive
 * mode.
 *
 * <file> may also be a glob pattern, in which case
 * all regular files matching the pattern are opened/edited.
 * Globbing is performed exactly the same as the
 * \fBEN\fP command does.
 * Also refer to the section called
 * .B Glob Patterns
 * for more details.
 *
 * File names of buffers in the ring are normalized
 * by making them absolute.
 * Any comparison on file names is performed using
 * guessed or actual absolute file paths, so that
 * one file may be referred to in many different ways
 * (paths).
 *
 * <file> does not have to exist on disk.
 * In this case, an empty buffer is created and its
 * name is guessed from <file>.
 * When the newly created buffer is first saved,
 * the file is created on disk and the buffer's name
 * will be updated to the absolute path of the file
 * on disk.
 *
 * File names may also be tab-completed and string building
 * characters are enabled by default.
 *
 * If <n> is greater than zero, the string argument
 * must be empty.
 * Instead <n> selects a buffer from the ring to edit.
 * A value of 1 denotes the first buffer, 2 the second,
 * ecetera.
 */
TECO_DEFINE_STATE_EXPECTFILE(teco_state_edit_file,
	.initial_cb = (teco_state_initial_cb_t)teco_state_edit_file_initial
);

static teco_state_t *
teco_state_save_file_done(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	if (ctx->mode > TECO_MODE_NORMAL)
		return &teco_state_start;

	g_autofree gchar *filename = teco_file_expand_path(str->data);
	if (teco_qreg_current) {
		if (!teco_qreg_save(teco_qreg_current, filename, error))
			return NULL;
	} else {
		if (!teco_buffer_save(teco_ring_current, *filename ? filename : NULL, error))
			return NULL;
	}

	return &teco_state_start;
}

/*$ EW write save
 * EW$ -- Save current buffer or Q-Register
 * EWfile$
 *
 * Saves the current buffer to disk.
 * If the buffer was dirty, it will be clean afterwards.
 * If the string argument <file> is not empty,
 * the buffer is saved with the specified file name
 * and is renamed in the ring.
 *
 * The EW command also works if the current document
 * is a Q-Register, i.e. a Q-Register is edited.
 * In this case, the string contents of the current
 * Q-Register are saved to <file>.
 * Q-Registers have no notion of associated file names,
 * so <file> must be always specified.
 *
 * In interactive mode, EW is executed immediately and
 * may be rubbed out.
 * In order to support that, \*(ST creates so called
 * save point files.
 * It does not merely overwrite existing files when saving
 * but moves them to save point files instead.
 * Save point files are called \(lq.teco-\fIn\fP-\fIfilename\fP~\(rq,
 * where <filename> is the name of the saved file and <n> is
 * a number that is increased with every save operation.
 * Save point files are always created in the same directory
 * as the original file to ensure that no copying of the file
 * on disk is necessary but only a rename of the file.
 * When rubbing out the EW command, \*(ST restores the latest
 * save point file by moving (renaming) it back to its
 * original path \(em also not requiring any on-disk copying.
 * \*(ST is impossible to crash, but just in case it still
 * does it may leave behind these save point files which
 * must be manually deleted by the user.
 * Otherwise save point files are deleted on command line
 * termination.
 *
 * File names may also be tab-completed and string building
 * characters are enabled by default.
 */
TECO_DEFINE_STATE_EXPECTFILE(teco_state_save_file);
