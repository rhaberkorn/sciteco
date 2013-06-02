/*
 * Copyright (C) 2012-2013 Robin Haberkorn
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

#include <string.h>
#include <bsd/sys/queue.h>

#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>

#include <Scintilla.h>

#include "sciteco.h"
#include "interface.h"
#include "undo.h"
#include "document.h"
#include "qregisters.h"
#include "parser.h"

/*
 * Auxiliary functions
 */
static inline bool
is_glob_pattern(const gchar *str)
{
	return strchr(str, '*') || strchr(str, '?');
}

/*
 * Get absolute/full version of a possibly relative path.
 * Works with existing and non-existing paths (in the latter case,
 * heuristics may be applied.)
 */
gchar *get_absolute_path(const gchar *path);

/*
 * Classes
 */

class Buffer : public TECODocument {
	class UndoTokenClose : public UndoToken {
		Buffer *buffer;

	public:
		UndoTokenClose(Buffer *_buffer)
			      : UndoToken(), buffer(_buffer) {}

		void run(void);
	};

public:
	TAILQ_ENTRY(Buffer) buffers;

	gchar *filename;
	gint savepoint_id;
	bool dirty;

	Buffer() : TECODocument(),
		   filename(NULL), savepoint_id(0), dirty(false) {}
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
		TECODocument::edit();
		interface.info_update(this);
	}
	inline void
	undo_edit(void)
	{
		interface.undo_info_update(this);
		TECODocument::undo_edit();
	}

	void load(const gchar *filename);

	inline void
	undo_close(void)
	{
		undo.push(new UndoTokenClose(this));
	}
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
			if (buffer)
				delete buffer;
		}

		void run(void);
	};

	class UndoTokenRemoveFile : public UndoToken {
		gchar *filename;

	public:
		UndoTokenRemoveFile(const gchar *_filename)
				   : filename(g_strdup(_filename)) {}
		~UndoTokenRemoveFile()
		{
			g_free(filename);
		}

		void
		run(void)
		{
			g_unlink(filename);
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

	Buffer *find(const gchar *filename);
	Buffer *find(tecoInt id);

	void dirtify(void);
	bool is_any_dirty(void);

	bool edit(tecoInt id);
	void edit(const gchar *filename);
	inline void
	undo_edit(void)
	{
		current->update();
		undo.push_var(QRegisters::current);
		undo.push_var(current)->undo_edit();
	}

	bool save(const gchar *filename);

	void close(Buffer *buffer);
	void close(void);
	inline void
	undo_close(void)
	{
		current->undo_close();
	}
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
	State *done(const gchar *str);
};

class StateSaveFile : public StateExpectFile {
private:
	State *done(const gchar *str);
};

namespace States {
	extern StateEditFile	editfile;
	extern StateSaveFile	savefile;
}

/* FIXME: clean up current_doc_update() usage */
static inline void
current_doc_update(void)
{
	if (ring.current)
		ring.current->update();
	else if (QRegisters::current)
		QRegisters::current->update_string();
}

#endif
