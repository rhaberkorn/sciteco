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

#ifndef __QREGISTERS_H
#define __QREGISTERS_H

#include <string.h>

#include <bsd/sys/queue.h>

#include <glib.h>
#include <glib/gprintf.h>

#include <Scintilla.h>

#include "sciteco.h"
#include "error.h"
#include "interface.h"
#include "ioview.h"
#include "undo.h"
#include "rbtree.h"
#include "parser.h"
#include "document.h"

namespace SciTECO {

namespace QRegisters {
	/* initialized after Interface::main() in main() */
	extern IOView view;
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

	virtual void set_string(const gchar *str, gsize len);
	inline void
	set_string(const gchar *str)
	{
		set_string(str, str ? strlen(str) : 0);
	}
	virtual void undo_set_string(void);

	virtual void append_string(const gchar *str, gsize len);
	inline void
	append_string(const gchar *str)
	{
		append_string(str, str ? strlen(str) : 0);
	}
	virtual inline void
	undo_append_string(void)
	{
		undo_set_string();
	}
	virtual gchar *get_string(void);
	virtual gsize get_string_size(void);
	virtual gint get_character(gint position);

	virtual void
	exchange_string(QRegisterData &reg)
	{
		string.exchange(reg.string);
	}
	virtual void undo_exchange_string(QRegisterData &reg);

	/*
	 * The QRegisterStack must currently still access the
	 * string fields directly to exchange data efficiently.
	 */
	friend class QRegisterStack;
};

class QRegister : public RBTreeString::RBEntryOwnString, public QRegisterData {
protected:
	/**
	 * The default constructor for subclasses.
	 * This leaves the name uninitialized.
	 */
	QRegister() {}

public:
	QRegister(const gchar *name)
		 : RBTreeString::RBEntryOwnString(name) {}

	virtual void edit(void);
	virtual void undo_edit(void);

	void execute(bool locals = true);

	void undo_set_eol_mode(void);
	void set_eol_mode(gint mode);

	/*
	 * Load and save already care about undo token
	 * creation.
	 */
	void load(const gchar *filename);
	void save(const gchar *filename);
};

class QRegisterBufferInfo : public QRegister {
public:
	QRegisterBufferInfo() : QRegister("*") {}

	/* setting "*" is equivalent to nEB */
	tecoInt set_integer(tecoInt v);
	void undo_set_integer(void);

	tecoInt get_integer(void);

	void
	set_string(const gchar *str, gsize len)
	{
		throw QRegOpUnsupportedError(name);
	}
	void undo_set_string(void) {}

	void
	append_string(const gchar *str, gsize len)
	{
		throw QRegOpUnsupportedError(name);
	}
	void undo_append_string(void) {}

	gchar *get_string(void);
	gsize get_string_size(void);
	gint get_character(gint pos);

	void edit(void);
};

class QRegisterWorkingDir : public QRegister {
public:
	QRegisterWorkingDir() : QRegister("$") {}

	void set_string(const gchar *str, gsize len);
	void undo_set_string(void);

	void
	append_string(const gchar *str, gsize len)
	{
		throw QRegOpUnsupportedError(name);
	}
	void undo_append_string(void) {}

	gchar *get_string(void);
	gsize get_string_size(void);
	gint get_character(gint pos);

	void edit(void);

	void exchange_string(QRegisterData &reg);
	void undo_exchange_string(QRegisterData &reg);
};

class QRegisterClipboard : public QRegister {
	class UndoTokenSetClipboard : public UndoToken {
		gchar *name;
		gchar *str;
		gsize str_len;

	public:
		/**
		 * Construct undo token.
		 *
		 * This passes ownership of the clipboard content string
		 * to the undo token object.
		 */
		UndoTokenSetClipboard(const gchar *_name, gchar *_str, gsize _str_len)
		                     : name(g_strdup(_name)), str(_str), str_len(_str_len) {}
		~UndoTokenSetClipboard()
		{
			g_free(str);
			g_free(name);
		}

		void run(void);

		gsize
		get_size(void) const
		{
			return sizeof(*this) + strlen(name) + str_len;
		}
	};

	/**
	 * Gets the clipboard name.
	 * Can be easily derived from the Q-Register name.
	 */
	inline const gchar *
	get_clipboard_name(void) const
	{
		return name+1;
	}

public:
	QRegisterClipboard(const gchar *_name = NULL)
	{
		name = g_strconcat("~", _name, NIL);
	}

	void set_string(const gchar *str, gsize len);
	void undo_set_string(void);

	/*
	 * FIXME: We could support that.
	 */
	void
	append_string(const gchar *str, gsize len)
	{
		throw QRegOpUnsupportedError(name);
	}
	void undo_append_string(void) {}

	gchar *get_string(gsize *out_len);
	gchar *get_string(void);
	gsize get_string_size(void);
	gint get_character(gint pos);

	void edit(void);

	void exchange_string(QRegisterData &reg);
	void undo_exchange_string(QRegisterData &reg);
};

class QRegisterTable : private RBTreeString {
	class UndoTokenRemove : public UndoTokenWithSize<UndoTokenRemove> {
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
			undo.push<UndoTokenRemove>(this, reg);
	}

	inline QRegister *
	insert(QRegister *reg)
	{
		reg->must_undo = must_undo;
		RBTreeString::insert(reg);
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
	find(const gchar *name)
	{
		return (QRegister *)RBTreeString::find(name);
	}
	inline QRegister *
	operator [](const gchar *name)
	{
		return find(name);
	}
	inline QRegister *
	operator [](gchar chr)
	{
		gchar buf[] = {chr, '\0'};
		return find(buf);
	}

	inline QRegister *
	nfind(const gchar *name)
	{
		return (QRegister *)RBTreeString::nfind(name);
	}

	void edit(QRegister *reg);
	inline QRegister *
	edit(const gchar *name)
	{
		QRegister *reg = find(name);

		if (!reg)
			return NULL;
		edit(reg);
		return reg;
	}

	void set_environ(void);
	gchar **get_environ(void);
	void update_environ(void);

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
			delete entry;
		}

		void run(void);

		gsize
		get_size(void) const
		{
			return entry ? sizeof(*this) + sizeof(*entry)
			             : sizeof(*this);
		}
	};

	class UndoTokenPop : public UndoTokenWithSize<UndoTokenPop> {
		QRegisterStack *stack;

	public:
		UndoTokenPop(QRegisterStack *_stack)
			    : stack(_stack) {}

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

enum QRegSpecType {
	/** Register must exist, else fail */
	QREG_REQUIRED,
	/**
	 * Return NULL if register does not exist.
	 * You can still call QRegSpecMachine::fail() to require it.
	 */
	QREG_OPTIONAL,
	/** Initialize register if it does not already exist */
	QREG_OPTIONAL_INIT
};

class QRegSpecMachine : public MicroStateMachine<QRegister *> {
	StringBuildingMachine string_machine;
	QRegSpecType type;

	bool is_local;
	gint nesting;
	gchar *name;

public:
	QRegSpecMachine(QRegSpecType _type = QREG_REQUIRED)
		       : MicroStateMachine<QRegister *>(),
			 type(_type),
			 is_local(false), nesting(0), name(NULL) {}

	~QRegSpecMachine()
	{
		g_free(name);
	}

	void reset(void);

	bool input(gchar chr, QRegister *&result);

	inline void
	fail(void) G_GNUC_NORETURN
	{
		throw InvalidQRegError(name, is_local);
	}
};

/*
 * Command states
 */

/*
 * Super class for states accepting Q-Register specifications
 */
class StateExpectQReg : public State {
public:
	StateExpectQReg(QRegSpecType type = QREG_REQUIRED);

private:
	State *custom(gchar chr);

protected:
	QRegSpecMachine machine;

	virtual State *got_register(QRegister *reg) = 0;
};

class StatePushQReg : public StateExpectQReg {
private:
	State *got_register(QRegister *reg);
};

class StatePopQReg : public StateExpectQReg {
public:
	StatePopQReg() : StateExpectQReg(QREG_OPTIONAL_INIT) {}

private:
	State *got_register(QRegister *reg);
};

class StateEQCommand : public StateExpectQReg {
public:
	StateEQCommand() : StateExpectQReg(QREG_OPTIONAL_INIT) {}

private:
	State *got_register(QRegister *reg);
};

class StateLoadQReg : public StateExpectFile {
private:
	State *got_file(const gchar *filename);
};

class StateEPctCommand : public StateExpectQReg {
private:
	State *got_register(QRegister *reg);
};

class StateSaveQReg : public StateExpectFile {
private:
	State *got_file(const gchar *filename);
};

class StateQueryQReg : public State {
	QRegSpecMachine machine;

public:
	StateQueryQReg();

private:
	State *custom(gchar chr);
};

class StateCtlUCommand : public StateExpectQReg {
public:
	StateCtlUCommand() : StateExpectQReg(QREG_OPTIONAL_INIT) {}

private:
	State *got_register(QRegister *reg);
};

class StateEUCommand : public StateExpectQReg {
public:
	StateEUCommand() : StateExpectQReg(QREG_OPTIONAL_INIT) {}

private:
	State *got_register(QRegister *reg);
};

class StateSetQRegString : public StateExpectString {
	bool text_added;

public:
	StateSetQRegString(bool building)
	                  : StateExpectString(building) {}

private:
	void initial(void);
	State *done(const gchar *str);
};

class StateGetQRegString : public StateExpectQReg {
private:
	State *got_register(QRegister *reg);
};

class StateSetQRegInteger : public StateExpectQReg {
public:
	StateSetQRegInteger() : StateExpectQReg(QREG_OPTIONAL_INIT) {}

private:
	State *got_register(QRegister *reg);
};

class StateIncreaseQReg : public StateExpectQReg {
public:
	StateIncreaseQReg() : StateExpectQReg(QREG_OPTIONAL_INIT) {}

private:
	State *got_register(QRegister *reg);
};

class StateMacro : public StateExpectQReg {
private:
	State *got_register(QRegister *reg);
};

class StateMacroFile : public StateExpectFile {
private:
	State *got_file(const gchar *filename);
};

class StateCopyToQReg : public StateExpectQReg {
public:
	StateCopyToQReg() : StateExpectQReg(QREG_OPTIONAL_INIT) {}

private:
	State *got_register(QRegister *reg);
};

namespace States {
	extern StatePushQReg		pushqreg;
	extern StatePopQReg		popqreg;
	extern StateEQCommand		eqcommand;
	extern StateLoadQReg		loadqreg;
	extern StateEPctCommand		epctcommand;
	extern StateSaveQReg		saveqreg;
	extern StateQueryQReg		queryqreg;
	extern StateCtlUCommand		ctlucommand;
	extern StateEUCommand		eucommand;
	extern StateSetQRegString	setqregstring_nobuilding;
	extern StateSetQRegString	setqregstring_building;
	extern StateGetQRegString	getqregstring;
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
