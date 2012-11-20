#ifndef __QBUFFERS_H
#define __QBUFFERS_H

#include <string.h>
#include <bsd/sys/queue.h>

#include <glib.h>
#include <glib/gprintf.h>

#include <Scintilla.h>

#include "sciteco.h"
#include "interface.h"
#include "undo.h"
#include "rbtree.h"
#include "parser.h"

/*
 * Auxiliary functions
 */
static inline bool
is_glob_pattern(const gchar *str)
{
	return strchr(str, '*') || strchr(str, '?');
}

gchar *get_absolute_path(const gchar *path);

/*
 * Classes
 */

class QRegisterData {
	gint64 integer;

public:
	typedef void document;
	document *string;
	gint dot;

	/*
	 * whether to generate UndoTokens (unnecessary in macro invocations)
	 */
	bool must_undo;

	QRegisterData() : integer(0), string(NULL), dot(0), must_undo(true) {}
	virtual
	~QRegisterData()
	{
		if (string)
			interface.ssm(SCI_RELEASEDOCUMENT, 0, (sptr_t)string);
	}

	inline document *
	get_document(void)
	{
		if (!string)
			string = (document *)interface.ssm(SCI_CREATEDOCUMENT);
		return string;
	}

	virtual gint64
	set_integer(gint64 i)
	{
		return integer = i;
	}
	virtual void
	undo_set_integer(void)
	{
		if (must_undo)
			undo.push_var(integer);
	}
	virtual gint64
	get_integer(void)
	{
		return integer;
	}

	virtual void set_string(const gchar *str);
	virtual void undo_set_string(void);
	virtual gchar *get_string(void);

	virtual void edit(void);
	virtual void undo_edit(void);
};

class QRegister : public RBTree::RBEntry, public QRegisterData {
public:
	gchar *name;

	QRegister(const gchar *_name)
		 : QRegisterData(), name(g_strdup(_name)) {}
	virtual
	~QRegister()
	{
		g_free(name);
	}

	int
	operator <(RBEntry &entry)
	{
		return g_strcmp0(name, ((QRegister &)entry).name);
	}

	virtual void edit(void);
	virtual void undo_edit(void);

	void execute(bool locals = true) throw (State::Error);

	bool load(const gchar *filename);
	inline void
	undo_load(void)
	{
		undo_set_string();
	}
};

class QRegisterBufferInfo : public QRegister {
public:
	QRegisterBufferInfo() : QRegister("*")
	{
		get_document();
	}

	void set_string(const gchar *str) {}
	void undo_set_string(void) {}
	gchar *get_string(void);

	void edit(void);
};

class QRegisterTable : public RBTree {
	bool must_undo;

public:
	QRegisterTable(bool _undo = true) : RBTree(), must_undo(_undo) {}

	inline QRegister *
	insert(QRegister *reg)
	{
		reg->must_undo = must_undo;
		RBTree::insert(reg);
		return reg;
	}

	inline void
	initialize(const gchar *name)
	{
		QRegister *reg = new QRegister(name);
		insert(reg);
		/* make sure document is initialized */
		reg->get_document();
	}
	inline void
	initialize(gchar name)
	{
		initialize((gchar []){name, '\0'});
	}
	void initialize(void);

	inline QRegister *
	operator [](const gchar *name)
	{
		QRegister reg(name);
		return (QRegister *)find(&reg);
	}
	inline QRegister *
	operator [](gchar chr)
	{
		return operator []((gchar []){chr, '\0'});
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
};

class QRegisterStack {
	class Entry : public QRegisterData {
	public:
		SLIST_ENTRY(Entry) entries;

		Entry() : QRegisterData() {}
	};

	class UndoTokenPush : public UndoToken {
		QRegisterStack *stack;
		/* only remaining reference to stack entry */
		Entry *entry;

	public:
		UndoTokenPush(QRegisterStack *_stack, Entry *_entry)
			     : UndoToken(), stack(_stack), entry(_entry) {}

		~UndoTokenPush()
		{
			if (entry)
				delete entry;
		}

		void run(void);
	};

	class UndoTokenPop : public UndoToken {
		QRegisterStack *stack;

	public:
		UndoTokenPop(QRegisterStack *_stack)
			    : UndoToken(), stack(_stack) {}

		void run(void);
	};

	SLIST_HEAD(Head, Entry) head;

public:
	QRegisterStack()
	{
		SLIST_INIT(&head);
	}
	~QRegisterStack();

	void push(QRegister *reg);
	bool pop(QRegister *reg);
};

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
			/* NOTE: the buffer is NOT deleted on Token destruction */
			delete buffer;
		}
	};

public:
	LIST_ENTRY(Buffer) buffers;

	gchar *filename;
	gint dot;

	gint savepoint_id;

	bool dirty;

private:
	typedef void document;
	document *doc;

public:
	Buffer() : filename(NULL), dot(0), savepoint_id(0), dirty(false)
	{
		doc = (document *)interface.ssm(SCI_CREATEDOCUMENT);
	}
	~Buffer()
	{
		interface.ssm(SCI_RELEASEDOCUMENT, 0, (sptr_t)doc);
		g_free(filename);
	}

	inline Buffer *&
	next(void)
	{
		return LIST_NEXT(this, buffers);
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
		interface.ssm(SCI_SETDOCPOINTER, 0, (sptr_t)doc);
		interface.ssm(SCI_GOTOPOS, dot);
		interface.info_update(this);
	}
	inline void
	undo_edit(void)
	{
		interface.undo_info_update(this);
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

	void dirtify(void);
	bool is_any_dirty(void);

	void edit(const gchar *filename);
	inline void
	undo_edit(void)
	{
		current->dot = interface.ssm(SCI_GETCURRENTPOS);
		undo.push_var(current);
		current->undo_edit();
	}

	bool save(const gchar *filename);

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

class StateEditFile : public StateExpectString {
private:
	void do_edit(const gchar *filename);

	void initial(void) throw (Error);
	State *done(const gchar *str) throw (Error);
};

class StateSaveFile : public StateExpectString {
private:
	State *done(const gchar *str) throw (Error);
};

class StatePushQReg : public StateExpectQReg {
private:
	State *got_register(QRegister *reg) throw (Error);
};

class StatePopQReg : public StateExpectQReg {
private:
	State *got_register(QRegister *reg) throw (Error);
};

class StateEQCommand : public StateExpectQReg {
private:
	State *got_register(QRegister *reg) throw (Error);
};

class StateLoadQReg : public StateExpectString {
private:
	State *done(const gchar *str) throw (Error);
};

class StateCtlUCommand : public StateExpectQReg {
private:
	State *got_register(QRegister *reg) throw (Error);
};

class StateSetQRegString : public StateExpectString {
public:
	StateSetQRegString() : StateExpectString(false) {}
private:
	State *done(const gchar *str) throw (Error);
};

class StateGetQRegInteger : public StateExpectQReg {
private:
	State *got_register(QRegister *reg) throw (Error);
};

class StateSetQRegInteger : public StateExpectQReg {
private:
	State *got_register(QRegister *reg) throw (Error);
};

class StateIncreaseQReg : public StateExpectQReg {
private:
	State *got_register(QRegister *reg) throw (Error);
};

class StateMacro : public StateExpectQReg {
private:
	State *got_register(QRegister *reg) throw (Error);
};

class StateCopyToQReg : public StateExpectQReg {
private:
	State *got_register(QRegister *reg) throw (Error);
};

namespace States {
	extern StateEditFile		editfile;
	extern StateSaveFile		savefile;

	extern StatePushQReg		pushqreg;
	extern StatePopQReg		popqreg;
	extern StateEQCommand		eqcommand;
	extern StateLoadQReg		loadqreg;
	extern StateCtlUCommand		ctlucommand;
	extern StateSetQRegString	setqregstring;
	extern StateGetQRegInteger	getqreginteger;
	extern StateSetQRegInteger	setqreginteger;
	extern StateIncreaseQReg	increaseqreg;
	extern StateMacro		macro;
	extern StateCopyToQReg		copytoqreg;
}

namespace QRegisters {
	extern QRegisterTable	globals;
	extern QRegisterTable	*locals;

	enum Hook {
		HOOK_ADD = 1,
		HOOK_EDIT,
		HOOK_CLOSE,
		HOOK_QUIT
	};
	void hook(Hook type);
}

#endif
