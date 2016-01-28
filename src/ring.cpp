/*
 * Copyright (C) 2012-2016 Robin Haberkorn
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

#include <bsd/sys/queue.h>

#include <glib.h>
#include <glib/gprintf.h>

#include <Scintilla.h>

#include "sciteco.h"
#include "interface.h"
#include "ioview.h"
#include "undo.h"
#include "parser.h"
#include "expressions.h"
#include "qregisters.h"
#include "glob.h"
#include "error.h"
#include "ring.h"

namespace SciTECO {

namespace States {
	StateEditFile	editfile;
	StateSaveFile	savefile;
}

void
Buffer::UndoTokenClose::run(void)
{
	ring.close(buffer);
	/* NOTE: the buffer is NOT deleted on Token destruction */
	delete buffer;
}

void
Buffer::save(const gchar *filename)
{
	if (!filename && !Buffer::filename)
		throw Error("File name expected");

	IOView::save(filename ? : Buffer::filename);

	/*
	 * Undirtify
	 * NOTE: info update is performed by set_filename()
	 */
	interface.undo_info_update(this);
	undo.push_var(dirty) = false;

	/*
	 * FIXME: necessary also if the filename was not specified but the file
	 * is (was) new, in order to canonicalize the filename.
	 * May be circumvented by cananonicalizing without requiring the file
	 * name to exist (like readlink -f)
	 * NOTE: undo_info_update is already called above
	 */
	undo.push_str(Buffer::filename);
	set_filename(filename ? : Buffer::filename);
}

void
Ring::UndoTokenEdit::run(void)
{
	/*
	 * assumes that buffer still has correct prev/next
	 * pointers
	 */
	if (buffer->next())
		TAILQ_INSERT_BEFORE(buffer->next(), buffer, buffers);
	else
		TAILQ_INSERT_TAIL(&ring->head, buffer, buffers);

	ring->current = buffer;
	buffer->edit();
	buffer = NULL;
}

tecoInt
Ring::get_id(Buffer *buffer)
{
	tecoInt ret = 0;
	Buffer *cur;

	TAILQ_FOREACH(cur, &head, buffers) {
		ret++;
		if (cur == buffer)
			break;
	}

	return ret;
}

Buffer *
Ring::find(const gchar *filename)
{
	gchar *resolved = get_absolute_path(filename);
	Buffer *cur;

	TAILQ_FOREACH(cur, &head, buffers)
		if (!g_strcmp0(cur->filename, resolved))
			break;

	g_free(resolved);
	return cur;
}

Buffer *
Ring::find(tecoInt id)
{
	Buffer *cur;

	TAILQ_FOREACH(cur, &head, buffers)
		if (!--id)
			break;

	return cur;
}

void
Ring::dirtify(void)
{
	if (QRegisters::current || current->dirty)
		return;

	interface.undo_info_update(current);
	undo.push_var(current->dirty);
	current->dirty = true;
	interface.info_update(current);
}

bool
Ring::is_any_dirty(void)
{
	Buffer *cur;

	TAILQ_FOREACH(cur, &head, buffers)
		if (cur->dirty)
			return true;

	return false;
}

void
Ring::save_all_dirty_buffers(void)
{
	Buffer *cur;

	TAILQ_FOREACH(cur, &head, buffers)
		if (cur->dirty && cur->filename)
			cur->save();
}

bool
Ring::edit(tecoInt id)
{
	Buffer *buffer = find(id);

	if (!buffer)
		return false;

	QRegisters::current = NULL;
	current = buffer;
	buffer->edit();

	QRegisters::hook(QRegisters::HOOK_EDIT);

	return true;
}

void
Ring::edit(const gchar *filename)
{
	Buffer *buffer = find(filename);

	QRegisters::current = NULL;
	if (buffer) {
		current = buffer;
		buffer->edit();

		QRegisters::hook(QRegisters::HOOK_EDIT);
	} else {
		buffer = new Buffer();
		TAILQ_INSERT_TAIL(&head, buffer, buffers);

		current = buffer;
		undo_close();

		if (filename && g_file_test(filename, G_FILE_TEST_IS_REGULAR)) {
			buffer->edit();
			buffer->load(filename);

			interface.msg(InterfaceCurrent::MSG_INFO,
				      "Added file \"%s\" to ring", filename);
		} else {
			buffer->edit();
			buffer->set_filename(filename);

			if (filename)
				interface.msg(InterfaceCurrent::MSG_INFO,
					      "Added new file \"%s\" to ring",
					      filename);
			else
				interface.msg(InterfaceCurrent::MSG_INFO,
					      "Added new unnamed file to ring.");
		}

		QRegisters::hook(QRegisters::HOOK_ADD);
	}
}

void
Ring::close(Buffer *buffer)
{
	TAILQ_REMOVE(&head, buffer, buffers);

	if (buffer->filename)
		interface.msg(InterfaceCurrent::MSG_INFO,
			      "Removed file \"%s\" from the ring",
			      buffer->filename);
	else
		interface.msg(InterfaceCurrent::MSG_INFO,
			      "Removed unnamed file from the ring.");
}

void
Ring::close(void)
{
	Buffer *buffer = current;

	QRegisters::hook(QRegisters::HOOK_CLOSE);
	close(buffer);
	current = buffer->next() ? : buffer->prev();
	/* transfer responsibility to UndoToken object */
	undo.push(new UndoTokenEdit(this, buffer));

	if (current) {
		current->edit();
		QRegisters::hook(QRegisters::HOOK_EDIT);
	} else {
		edit((const gchar *)NULL);
	}
}

void
Ring::set_scintilla_undo(bool state)
{
	Buffer *cur;

	TAILQ_FOREACH(cur, &head, buffers)
		cur->set_scintilla_undo(state);
}

Ring::~Ring()
{
	Buffer *buffer, *next;

	TAILQ_FOREACH_SAFE(buffer, &head, buffers, next)
		delete buffer;
}

/*
 * Command states
 */

void
StateEditFile::do_edit(const gchar *filename)
{
	current_doc_undo_edit();
	ring.edit(filename);
}

void
StateEditFile::do_edit(tecoInt id)
{
	current_doc_undo_edit();
	if (!ring.edit(id))
		throw Error("Invalid buffer id %" TECO_INTEGER_FORMAT, id);
}

/*$
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
 * EN command does.
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
void
StateEditFile::initial(void)
{
	tecoInt id = expressions.pop_num_calc(0, -1);

	allowFilename = true;

	if (id == 0) {
		for (Buffer *cur = ring.first(); cur; cur = cur->next())
			interface.popup_add(InterfaceCurrent::POPUP_FILE,
					    cur->filename ? : "(Unnamed)",
					    cur == ring.current);

		interface.popup_show();
	} else if (id > 0) {
		allowFilename = false;
		do_edit(id);
	}
}

State *
StateEditFile::got_file(const gchar *filename)
{
	BEGIN_EXEC(&States::start);

	if (!allowFilename) {
		if (*filename)
			throw Error("If a buffer is selected by id, the <EB> "
				    "string argument must be empty");

		return &States::start;
	}

	if (is_glob_pattern(filename)) {
		Globber globber(filename, G_FILE_TEST_IS_REGULAR);
		gchar *globbed_filename;

		while ((globbed_filename = globber.next()))
			do_edit(globbed_filename);
	} else {
		do_edit(*filename ? filename : NULL);
	}

	return &States::start;
}

/*$
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
State *
StateSaveFile::got_file(const gchar *filename)
{
	BEGIN_EXEC(&States::start);

	if (QRegisters::current)
		QRegisters::current->save(filename);
	else
		ring.current->save(*filename ? filename : NULL);

	return &States::start;
}

void
current_doc_undo_edit(void)
{
	if (!QRegisters::current)
		ring.undo_edit();
	else
		undo.push_var(QRegisters::current)->undo_edit();
}

} /* namespace SciTECO */
