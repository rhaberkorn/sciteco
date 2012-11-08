#ifndef __QBUFFERS_H
#define __QBUFFERS_H

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
	Buffer *current;

public:
	Ring() : current(NULL)
	{
		LIST_INIT(&head);
	}
	~Ring();

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

	State *done(const gchar *str);
};

#endif
