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

#ifndef __RING_H
#define __RING_H

#include <bsd/sys/queue.h>

#include <glib.h>

#include <Scintilla.h>

#include "sciteco.h"
#include "interface.h"
#include "undo.h"
#include "qregisters.h"
#include "parser.h"
#include "ioview.h"

namespace SciTECO {

/*
 * Classes
 */

class Buffer : private IOView {
	TAILQ_ENTRY(Buffer) buffers;

	class UndoTokenClose : public UndoTokenWithSize<UndoTokenClose> {
		Buffer *buffer;

	public:
		UndoTokenClose(Buffer *_buffer)
			      : buffer(_buffer) {}

		void run(void);
	};

	inline void
	undo_close(void)
	{
		undo.push(new UndoTokenClose(this));
	}

public:
	gchar *filename;
	bool dirty;

	Buffer() : filename(NULL), dirty(false)
	{
		initialize();
		/* only have to do this once: */
		set_representations();
	}

	~Buffer()
	{
		g_free(filename);
	}

	inline Buffer *&
	next(void)
	{
		return TAILQ_NEXT(this, buffers);
	}
	inline Buffer *&
	prev(void)
	{
		TAILQ_HEAD(Head, Buffer);

		return TAILQ_PREV(this, Head, buffers);
	}

	inline void
	set_filename(const gchar *filename)
	{
		gchar *resolved = get_absolute_path(filename);
		g_free(Buffer::filename);
		Buffer::filename = resolved;
		interface.info_update(this);
	}

	inline void
	edit(void)
	{
		interface.show_view(this);
		interface.info_update(this);
	}
	inline void
	undo_edit(void)
	{
		interface.undo_info_update(this);
		interface.undo_show_view(this);
	}

	inline void
	load(const gchar *filename)
	{
		IOView::load(filename);

#if 0		/* NOTE: currently buffer cannot be dirty */
		interface.undo_info_update(this);
		undo.push_var(dirty) = false;
#endif

		set_filename(filename);
	}
	void save(const gchar *filename = NULL);

	/*
	 * Ring manages the buffer list and has privileged
	 * access.
	 */
	friend class Ring;
};

/* object declared in main.cpp */
extern class Ring {
	/*
	 * Emitted after a buffer close
	 * The pointer is the only remaining reference to the buffer!
	 */
	class UndoTokenEdit : public UndoToken {
		Ring	*ring;
		Buffer	*buffer;

	public:
		UndoTokenEdit(Ring *_ring, Buffer *_buffer)
			     : UndoToken(), ring(_ring), buffer(_buffer) {}
		~UndoTokenEdit()
		{
			delete buffer;
		}

		void run(void);

		gsize
		get_size(void) const
		{
			return buffer ? sizeof(*this) + sizeof(*buffer)
			              : sizeof(*this);
		}
	};

	TAILQ_HEAD(Head, Buffer) head;

public:
	Buffer *current;

	Ring() : current(NULL)
	{
		TAILQ_INIT(&head);
	}
	~Ring();

	inline Buffer *
	first(void)
	{
		return TAILQ_FIRST(&head);
	}
	inline Buffer *
	last(void)
	{
		return TAILQ_LAST(&head, Head);
	}

	tecoInt get_id(Buffer *buffer);
	inline tecoInt
	get_id(void)
	{
		return get_id(current);
	}

	Buffer *find(const gchar *filename);
	Buffer *find(tecoInt id);

	void dirtify(void);
	bool is_any_dirty(void);

	bool edit(tecoInt id);
	void edit(const gchar *filename);
	inline void
	undo_edit(void)
	{
		undo.push_var(QRegisters::current);
		undo.push_var(current)->undo_edit();
	}

	void close(Buffer *buffer);
	void close(void);
	inline void
	undo_close(void)
	{
		current->undo_close();
	}

	void set_scintilla_undo(bool state);
} ring;

/*
 * Command states
 */

class StateEditFile : public StateExpectFile {
private:
	bool allowFilename;

	void do_edit(const gchar *filename);
	void do_edit(tecoInt id);

	void initial(void);
	State *got_file(const gchar *filename);
};

class StateSaveFile : public StateExpectFile {
private:
	State *got_file(const gchar *filename);
};

namespace States {
	extern StateEditFile	editfile;
	extern StateSaveFile	savefile;
}

/*
 * Helper functions applying to any current
 * document (whether a buffer or QRegister).
 * There's currently no better place to put them.
 */
void current_doc_undo_edit(void);

static inline bool
current_doc_must_undo(void)
{
	/*
	 * If there's no currently edited Q-Register
	 * we must be editing the current buffer
	 */
	return !QRegisters::current ||
	       QRegisters::current->must_undo;
}

} /* namespace SciTECO */

#endif
