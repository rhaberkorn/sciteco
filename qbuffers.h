#ifndef __QBUFFERS_H
#define __QBUFFERS_H

#include <string.h>
#include <bsd/sys/queue.h>

#include <glib.h>
#include <glib/gprintf.h>

#include <Scintilla.h>

#include "sciteco.h"
#include "undo.h"
#include "rbtree.h"
#include "parser.h"

class QRegister : public RBTree::RBEntry {
public:
	gchar *name;

	gint64 integer;

	typedef void document;
	document *string;
	gint dot;

	QRegister(const gchar *_name)
		 : name(g_strdup(_name)), integer(0), string(NULL), dot(0) {}
	~QRegister()
	{
		if (string)
			editor_msg(SCI_RELEASEDOCUMENT, 0, (sptr_t)string);
		g_free(name);
	}

	int
	operator <(RBEntry &entry)
	{
		return g_strcmp0(name, ((QRegister &)entry).name);
	}

	inline document *
	get_string(void)
	{
		if (!string)
			string = (document *)editor_msg(SCI_CREATEDOCUMENT);
		return string;
	}

	inline void
	edit(void)
	{
		editor_msg(SCI_SETDOCPOINTER, 0, (sptr_t)get_string());
		editor_msg(SCI_GOTOPOS, dot);
	}
	inline void
	undo_edit(void)
	{
		undo.push_msg(SCI_GOTOPOS, dot);
		undo.push_msg(SCI_SETDOCPOINTER, 0, (sptr_t)get_string());
	}

	bool load(const gchar *filename);
};

extern class QRegisterTable : public RBTree {
	inline void
	initialize_register(const gchar *name)
	{
		QRegister *reg = new QRegister(name);
		insert(reg);
		/* make sure document is initialized */
		reg->get_string();
	}

public:
	QRegister *current;

	QRegisterTable() : RBTree(), current(NULL) {}

	void initialize(void);

	inline QRegister *
	operator [](const gchar *name)
	{
		QRegister reg(name);
		return (QRegister *)find(&reg);
	}

	void edit(QRegister *reg);
	inline QRegister *
	edit(const gchar *name)
	{
		QRegister *reg = operator [](name);

		if (!reg)
			return NULL;
		edit(reg);
		return reg;
	}
	inline void
	undo_edit(void)
	{
		current->dot = editor_msg(SCI_GETCURRENTPOS);

		undo.push_var<QRegister*>(current);
		current->undo_edit();
	}
} qregisters;

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

class StateEQCommand : public StateExpectQReg {
private:
	State *got_register(QRegister *reg);
};

class StateLoadQReg : public StateExpectString {
private:
	State *done(const gchar *str);
};

namespace States {
	extern StateFile	file;
	extern StateEQCommand	eqcommand;
	extern StateLoadQReg	loadqreg;
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
