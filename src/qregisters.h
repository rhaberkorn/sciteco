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
#include "document.h"

/*
 * Classes
 */

class QRegisterData {
protected:
	tecoInt integer;
	TECODocument string;

public:
	/*
	 * whether to generate UndoTokens (unnecessary in macro invocations)
	 */
	bool must_undo;

	QRegisterData() : integer(0), must_undo(true) {}
	virtual ~QRegisterData() {}

	virtual tecoInt
	set_integer(tecoInt i)
	{
		return integer = i;
	}
	virtual void
	undo_set_integer(void)
	{
		if (must_undo)
			undo.push_var(integer);
	}
	virtual tecoInt
	get_integer(void)
	{
		return integer;
	}

	inline void
	update_string(void)
	{
		string.update();
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

	friend class QRegisterStack;
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

	void execute(bool locals = true) throw (State::Error, ReplaceCmdline);

	bool load(const gchar *filename);
	inline void
	undo_load(void)
	{
		undo_set_string();
	}
};

class QRegisterBufferInfo : public QRegister {
public:
	QRegisterBufferInfo() : QRegister("*") {}

	tecoInt
	set_integer(tecoInt v)
	{
		return v;
	}
	void undo_set_integer(void) {}

	tecoInt get_integer(void);

	void set_string(const gchar *str) {}
	void undo_set_string(void) {}
	void append_string(const gchar *str) {}
	void undo_append_string(void) {}

	gchar *get_string(void);

	void edit(void);
};

class QRegisterTable : public RBTree {
	class UndoTokenRemove : public UndoToken {
		QRegisterTable *table;
		QRegister *reg;

	public:
		UndoTokenRemove(QRegisterTable *_table, QRegister *_reg)
			       : table(_table), reg(_reg) {}

		void
		run(void)
		{
			delete table->remove(reg);
		}
	};

	bool must_undo;

public:
	QRegisterTable(bool _undo = true);

	inline void
	undo_remove(QRegister *reg)
	{
		if (must_undo)
			undo.push(new UndoTokenRemove(this, reg));
	}

	inline QRegister *
	insert(QRegister *reg)
	{
		reg->must_undo = must_undo;
		RBTree::insert(reg);
		return reg;
	}
	inline QRegister *
	insert(const gchar *name)
	{
		return insert(new QRegister(name));
	}
	inline QRegister *
	insert(gchar name)
	{
		return insert(CHR2STR(name));
	}

	inline QRegister *
	operator [](const gchar *name)
	{
		QRegister reg(name);
		return (QRegister *)find(&reg);
	}
	inline QRegister *
	operator [](gchar chr)
	{
		return operator [](CHR2STR(chr));
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

	void push(QRegister &reg);
	bool pop(QRegister &reg);
};

class QRegSpecMachine : public MicroStateMachine<QRegister *> {
	StringBuildingMachine string_machine;
	bool initialize;

	bool is_local;
	gint nesting;
	gchar *name;

public:
	QRegSpecMachine(bool _init = false)
		       : MicroStateMachine<QRegister *>(),
			 initialize(_init),
			 is_local(false), nesting(0), name(NULL) {}

	~QRegSpecMachine()
	{
		g_free(name);
	}

	void reset(void);

	QRegister *input(gchar chr) throw (State::Error);
};

/*
 * Command states
 */

/*
 * Super class for states accepting Q-Register specifications
 */
class StateExpectQReg : public State {
	QRegSpecMachine machine;

public:
	StateExpectQReg(bool initialize = false);

private:
	State *custom(gchar chr) throw (Error, ReplaceCmdline);

protected:
	virtual State *got_register(QRegister &reg)
				   throw (Error, ReplaceCmdline) = 0;
};

class StatePushQReg : public StateExpectQReg {
private:
	State *got_register(QRegister &reg) throw (Error);
};

class StatePopQReg : public StateExpectQReg {
public:
	StatePopQReg() : StateExpectQReg(true) {}

private:
	State *got_register(QRegister &reg) throw (Error);
};

class StateEQCommand : public StateExpectQReg {
public:
	StateEQCommand() : StateExpectQReg(true) {}

private:
	State *got_register(QRegister &reg) throw (Error);
};

class StateLoadQReg : public StateExpectFile {
private:
	State *done(const gchar *str) throw (Error);
};

class StateCtlUCommand : public StateExpectQReg {
public:
	StateCtlUCommand() : StateExpectQReg(true) {}

private:
	State *got_register(QRegister &reg) throw (Error);
};

class StateSetQRegString : public StateExpectString {
public:
	StateSetQRegString() : StateExpectString(false) {}

private:
	State *done(const gchar *str) throw (Error);
};

class StateGetQRegString : public StateExpectQReg {
private:
	State *got_register(QRegister &reg) throw (Error);
};

class StateGetQRegInteger : public StateExpectQReg {
private:
	State *got_register(QRegister &reg) throw (Error);
};

class StateSetQRegInteger : public StateExpectQReg {
public:
	StateSetQRegInteger() : StateExpectQReg(true) {}

private:
	State *got_register(QRegister &reg) throw (Error);
};

class StateIncreaseQReg : public StateExpectQReg {
public:
	StateIncreaseQReg() : StateExpectQReg(true) {}

private:
	State *got_register(QRegister &reg) throw (Error);
};

class StateMacro : public StateExpectQReg {
private:
	State *got_register(QRegister &reg) throw (Error, ReplaceCmdline);
};

class StateMacroFile : public StateExpectFile {
private:
	State *done(const gchar *str) throw (Error, ReplaceCmdline);
};

class StateCopyToQReg : public StateExpectQReg {
public:
	StateCopyToQReg() : StateExpectQReg(true) {}

private:
	State *got_register(QRegister &reg) throw (Error);
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
	extern StateMacroFile		macro_file;
	extern StateCopyToQReg		copytoqreg;
}

namespace QRegisters {
	/* object declared in main.cpp */
	extern QRegisterTable	globals;
	extern QRegisterTable	*locals;
	extern QRegister	*current;

	void undo_edit(void);

	enum Hook {
		HOOK_ADD = 1,
		HOOK_EDIT,
		HOOK_CLOSE,
		HOOK_QUIT
	};
	void hook(Hook type);
}

#endif
