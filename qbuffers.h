#ifndef __QBUFFERS_H
#define __QBUFFERS_H

#include <string.h>
#include <bsd/sys/queue.h>

#include <glib.h>
#include <glib/gprintf.h>

#include <Scintilla.h>

#include "sciteco.h"
#include "undo.h"
#include "parser.h"

class Buffer {
	class UndoTokenClose : public UndoToken {
		Buffer *buffer;

	public:
		UndoTokenClose(Buffer *_buffer)
			      : UndoToken(), buffer(_buffer) {}

		void
		run(void)
		{
			buffer->close();
			delete buffer;
		}
	};

public:
	LIST_ENTRY(Buffer) buffers;

	gchar *filename;
	gint dot;

private:
	typedef void document;
	document *doc;

public:
	Buffer() : filename(NULL), dot(0)
	{
		doc = (document *)editor_msg(SCI_CREATEDOCUMENT);
	}
	~Buffer()
	{
		editor_msg(SCI_RELEASEDOCUMENT, 0, (sptr_t)doc);
		g_free(filename);
	}

	inline Buffer *
	next(void)
	{
		return LIST_NEXT(this, buffers);
	}

	inline void
	set_filename(const gchar *filename)
	{
		g_free(Buffer::filename);
		Buffer::filename = filename ? g_strdup(filename) : NULL;
	}

	inline void
	edit(void)
	{
		editor_msg(SCI_SETDOCPOINTER, 0, (sptr_t)doc);
		editor_msg(SCI_GOTOPOS, dot);
	}
	inline void
	undo_edit(void)
	{
		undo.push_msg(SCI_GOTOPOS, dot);
		undo.push_msg(SCI_SETDOCPOINTER, 0, (sptr_t)doc);
	}

	bool load(const gchar *filename);

	void close(void);
	inline void
	undo_close(void)
	{
		undo.push(new UndoTokenClose(this));
	}
};

extern class Ring {
	LIST_HEAD(Head, Buffer) head;

public:
	Buffer *current;

	Ring() : current(NULL)
	{
		LIST_INIT(&head);
	}
	~Ring();

	inline Buffer *
	first(void)
	{
		return LIST_FIRST(&head);
	}

	Buffer *find(const gchar *filename);

	bool edit(const gchar *filename);
	inline void
	undo_edit(void)
	{
		current->dot = editor_msg(SCI_GETCURRENTPOS);

		undo.push_var<Buffer*>(current);
		current->undo_edit();
	}

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

class StateFile : public StateExpectString {
private:
	void do_edit(const gchar *filename);

	void initial(void);
	State *done(const gchar *str);
};

namespace States {
	extern StateFile file;
}

/*
 * Auxiliary functions
 */
static inline bool
is_glob_pattern(const gchar *str)
{
	return strchr(str, '*') || strchr(str, '?');
}

#endif
