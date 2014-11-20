/*
 * Copyright (C) 2012-2014 Robin Haberkorn
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

namespace SciTECO {

namespace QRegisters {
	/* initialized after Interface.main() in main() */
	extern ViewCurrent view;
}

/*
 * Classes
 */

class QRegisterData {
protected:
	tecoInt integer;

	class QRegisterString : public Document {
	public:
		~QRegisterString()
		{
			release_document();
		}

	private:
		ViewCurrent &
		get_create_document_view(void)
		{
			return QRegisters::view;
		}
	} string;

public:
	/*
	 * Whether to generate UndoTokens (unnecessary in macro invocations).
	 *
	 * FIXME: Every QRegister has this field, but it only differs
	 * between local and global QRegisters. This wastes space.
	 * There must be a more clever way to inherit this property, e.g.
	 * by setting QRegisters::current_must_undo.
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

	virtual void set_string(const gchar *str);
	virtual void undo_set_string(void);
	virtual void append_string(const gchar *str);
	virtual inline void
	undo_append_string(void)
	{
		undo_set_string();
	}
	virtual gchar *get_string(void);

	/*
	 * The QRegisterStack must currently access the
	 * integer and string fields directly to exchange
	 * data efficiently.
	 */
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

	void execute(bool locals = true);

	void load(const gchar *filename);
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

class QRegisterTable : private RBTree {
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
		gchar buf[] = {name, '\0'};
		return insert(buf);
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
		gchar buf[] = {chr, '\0'};
		return operator [](buf);
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

	void clear(void);
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

	QRegister *input(gchar chr);
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
	State *custom(gchar chr);

protected:
	virtual State *got_register(QRegister &reg) = 0;
};

class StatePushQReg : public StateExpectQReg {
private:
	State *got_register(QRegister &reg);
};

class StatePopQReg : public StateExpectQReg {
public:
	StatePopQReg() : StateExpectQReg(true) {}

private:
	State *got_register(QRegister &reg);
};

class StateEQCommand : public StateExpectQReg {
public:
	StateEQCommand() : StateExpectQReg(true) {}

private:
	State *got_register(QRegister &reg);
};

class StateLoadQReg : public StateExpectFile {
private:
	State *done(const gchar *str);
};

class StateCtlUCommand : public StateExpectQReg {
public:
	StateCtlUCommand() : StateExpectQReg(true) {}

private:
	State *got_register(QRegister &reg);
};

class StateSetQRegString : public StateExpectString {
	bool text_added;

public:
	StateSetQRegString() : StateExpectString(false) {}

private:
	void initial(void);
	State *done(const gchar *str);
};

class StateGetQRegString : public StateExpectQReg {
private:
	State *got_register(QRegister &reg);
};

class StateGetQRegInteger : public StateExpectQReg {
private:
	State *got_register(QRegister &reg);
};

class StateSetQRegInteger : public StateExpectQReg {
public:
	StateSetQRegInteger() : StateExpectQReg(true) {}

private:
	State *got_register(QRegister &reg);
};

class StateIncreaseQReg : public StateExpectQReg {
public:
	StateIncreaseQReg() : StateExpectQReg(true) {}

private:
	State *got_register(QRegister &reg);
};

class StateMacro : public StateExpectQReg {
private:
	State *got_register(QRegister &reg);
};

class StateMacroFile : public StateExpectFile {
private:
	State *done(const gchar *str);
};

class StateCopyToQReg : public StateExpectQReg {
public:
	StateCopyToQReg() : StateExpectQReg(true) {}

private:
	State *got_register(QRegister &reg);
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

	enum Hook {
		HOOK_ADD = 1,
		HOOK_EDIT,
		HOOK_CLOSE,
		HOOK_QUIT
	};
	void hook(Hook type);
}

} /* namespace SciTECO */

#endif
