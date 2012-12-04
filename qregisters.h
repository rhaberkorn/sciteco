#ifndef __QREGISTERS_H
#define __QREGISTERS_H

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
	virtual void append_string(const gchar *str);
	virtual inline void
	undo_append_string(void)
	{
		undo_set_string();
	}
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

	gint64
	set_integer(gint64 v)
	{
		return v;
	}
	void undo_set_integer(void) {}

	gint64 get_integer(void);

	void set_string(const gchar *str) {}
	void undo_set_string(void) {}
	void append_string(const gchar *str) {}
	void undo_append_string(void) {}

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

/*
 * Command states
 */

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

class StateGetQRegString : public StateExpectQReg {
private:
	State *got_register(QRegister *reg) throw (Error);
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
	extern StatePushQReg		pushqreg;
	extern StatePopQReg		popqreg;
	extern StateEQCommand		eqcommand;
	extern StateLoadQReg		loadqreg;
	extern StateCtlUCommand		ctlucommand;
	extern StateSetQRegString	setqregstring;
	extern StateGetQRegString	getqregstring;
	extern StateGetQRegInteger	getqreginteger;
	extern StateSetQRegInteger	setqreginteger;
	extern StateIncreaseQReg	increaseqreg;
	extern StateMacro		macro;
	extern StateCopyToQReg		copytoqreg;
}

namespace QRegisters {
	extern QRegisterTable	globals;
	extern QRegisterTable	*locals;
	extern QRegister	*current;

	static inline void
	undo_edit(void)
	{
		current->dot = interface.ssm(SCI_GETCURRENTPOS);
		undo.push_var(current)->undo_edit();
	}

	enum Hook {
		HOOK_ADD = 1,
		HOOK_EDIT,
		HOOK_CLOSE,
		HOOK_QUIT
	};
	void hook(Hook type);
}

#endif
