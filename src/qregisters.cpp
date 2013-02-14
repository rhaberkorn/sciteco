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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <bsd/sys/queue.h>

#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>

#include <Scintilla.h>

#include "sciteco.h"
#include "interface.h"
#include "undo.h"
#include "parser.h"
#include "expressions.h"
#include "ring.h"
#include "qregisters.h"

namespace States {
	StatePushQReg		pushqreg;
	StatePopQReg		popqreg;
	StateEQCommand		eqcommand;
	StateLoadQReg		loadqreg;
	StateCtlUCommand	ctlucommand;
	StateSetQRegString	setqregstring;
	StateGetQRegString	getqregstring;
	StateGetQRegInteger	getqreginteger;
	StateSetQRegInteger	setqreginteger;
	StateIncreaseQReg	increaseqreg;
	StateMacro		macro;
	StateCopyToQReg		copytoqreg;
}

namespace QRegisters {
	QRegisterTable		globals INIT_PRIO(PRIO_INTERFACE+1);
	QRegisterTable		*locals = NULL;
	QRegister		*current = NULL;

	void
	undo_edit(void)
	{
		current->dot = interface.ssm(SCI_GETCURRENTPOS);
		undo.push_var(ring.current);
		undo.push_var(current)->undo_edit();
	}

	static QRegisterStack	stack;
}

static QRegister *register_argument = NULL;

static inline void
current_edit(void)
{
	if (ring.current)
		ring.current->edit();
	else if (QRegisters::current)
		QRegisters::current->edit();
}

void
QRegisterData::set_string(const gchar *str)
{
	edit();
	dot = 0;

	interface.ssm(SCI_BEGINUNDOACTION);
	interface.ssm(SCI_SETTEXT, 0, (sptr_t)(str ? : ""));
	interface.ssm(SCI_ENDUNDOACTION);

	current_edit();
}

void
QRegisterData::undo_set_string(void)
{
	/* set_string() assumes that dot has been saved */
	current_save_dot();

	if (!must_undo)
		return;

	if (ring.current)
		ring.current->undo_edit();
	else if (QRegisters::current)
		QRegisters::current->undo_edit();

	undo.push_var<gint>(dot);
	undo.push_msg(SCI_UNDO);

	undo_edit();
}

void
QRegisterData::append_string(const gchar *str)
{
	if (!str)
		return;

	edit();

	interface.ssm(SCI_BEGINUNDOACTION);
	interface.ssm(SCI_APPENDTEXT, strlen(str), (sptr_t)str);
	interface.ssm(SCI_ENDUNDOACTION);

	current_edit();
}

gchar *
QRegisterData::get_string(void)
{
	gint size;
	gchar *str;

	current_save_dot();
	edit();

	size = interface.ssm(SCI_GETLENGTH) + 1;
	str = (gchar *)g_malloc(size);
	interface.ssm(SCI_GETTEXT, size, (sptr_t)str);

	current_edit();

	return str;
}

void
QRegisterData::edit(void)
{
	interface.ssm(SCI_SETDOCPOINTER, 0, (sptr_t)get_document());
	interface.ssm(SCI_GOTOPOS, dot);
}

void
QRegisterData::undo_edit(void)
{
	if (!must_undo)
		return;

	undo.push_msg(SCI_GOTOPOS, dot);
	undo.push_msg(SCI_SETDOCPOINTER, 0, (sptr_t)get_document());
}

void
QRegister::edit(void)
{
	QRegisterData::edit();
	interface.info_update(this);
}

void
QRegister::undo_edit(void)
{
	if (!must_undo)
		return;

	interface.undo_info_update(this);
	QRegisterData::undo_edit();
}

void
QRegister::execute(bool locals) throw (State::Error, ReplaceCmdline)
{
	gchar *str = get_string();

	try {
		Execute::macro(str, locals);
	} catch (...) {
		g_free(str);
		throw; /* forward */
	}

	g_free(str);
}

bool
QRegister::load(const gchar *filename)
{
	gchar *contents;
	gsize size;

	/* FIXME: prevent excessive allocations by reading file into buffer */
	if (!g_file_get_contents(filename, &contents, &size, NULL))
		return false;

	edit();
	dot = 0;

	interface.ssm(SCI_BEGINUNDOACTION);
	interface.ssm(SCI_CLEARALL);
	interface.ssm(SCI_APPENDTEXT, size, (sptr_t)contents);
	interface.ssm(SCI_ENDUNDOACTION);

	g_free(contents);

	current_edit();

	return true;
}

gint64
QRegisterBufferInfo::get_integer(void)
{
	gint64 id = 1;

	if (!ring.current)
		return 0;

	for (Buffer *buffer = ring.first();
	     buffer != ring.current;
	     buffer = buffer->next())
		id++;

	return id;
}

gchar *
QRegisterBufferInfo::get_string(void)
{
	gchar *filename = ring.current ? ring.current->filename : NULL;

	return g_strdup(filename ? : "");
}

void
QRegisterBufferInfo::edit(void)
{
	gchar *filename = ring.current ? ring.current->filename : NULL;

	QRegister::edit();

	interface.ssm(SCI_BEGINUNDOACTION);
	interface.ssm(SCI_SETTEXT, 0, (sptr_t)(filename ? : ""));
	interface.ssm(SCI_ENDUNDOACTION);

	undo.push_msg(SCI_UNDO);
}

QRegisterTable::QRegisterTable(bool _undo) : RBTree(), must_undo(_undo)
{
	/* general purpose registers */
	for (gchar q = 'A'; q <= 'Z'; q++)
		initialize(q);
	for (gchar q = '0'; q <= '9'; q++)
		initialize(q);
}

void
QRegisterTable::edit(QRegister *reg)
{
	current_save_dot();
	reg->edit();

	ring.current = NULL;
	QRegisters::current = reg;
}

void
QRegisterStack::UndoTokenPush::run(void)
{
	SLIST_INSERT_HEAD(&stack->head, entry, entries);
	entry = NULL;
}

void
QRegisterStack::UndoTokenPop::run(void)
{
	Entry *entry = SLIST_FIRST(&stack->head);

	SLIST_REMOVE_HEAD(&stack->head, entries);
	delete entry;
}

void
QRegisterStack::push(QRegister &reg)
{
	Entry *entry = new Entry();

	entry->set_integer(reg.get_integer());
	if (reg.string) {
		gchar *str = reg.get_string();
		entry->set_string(str);
		g_free(str);
	}
	entry->dot = reg.dot;

	SLIST_INSERT_HEAD(&head, entry, entries);
	undo.push(new UndoTokenPop(this));
}

bool
QRegisterStack::pop(QRegister &reg)
{
	Entry *entry = SLIST_FIRST(&head);
	QRegisterData::document *string;

	if (!entry)
		return false;

	reg.undo_set_integer();
	reg.set_integer(entry->get_integer());

	/* exchange document ownership between Stack entry and Q-Register */
	string = reg.string;
	if (reg.must_undo)
		undo.push_var(reg.string);
	reg.string = entry->string;
	undo.push_var(entry->string);
	entry->string = string;

	if (reg.must_undo)
		undo.push_var(reg.dot);
	reg.dot = entry->dot;

	SLIST_REMOVE_HEAD(&head, entries);
	/* pass entry ownership to undo stack */
	undo.push(new UndoTokenPush(this, entry));

	return true;
}

QRegisterStack::~QRegisterStack()
{
	Entry *entry, *next;

	SLIST_FOREACH_SAFE(entry, &head, entries, next)
		delete entry;
}

void
QRegisters::hook(Hook type)
{
	if (!(Flags::ed & Flags::ED_HOOKS))
		return;

	expressions.push(type);
	globals["0"]->execute();
}

void
QRegSpecMachine::reset(void)
{
	MicroStateMachine::reset();
	string_machine.reset();
	undo.push_var(is_local) = false;
	undo.push_var(nesting) = 0;
	undo.push_str(name);
	g_free(name);
	name = NULL;
}

QRegister *
QRegSpecMachine::input(gchar chr) throw (State::Error)
{
	gchar *insert;

	if (state)
		goto *state;

	/* NULL state */
	switch (chr) {
	case '.': undo.push_var(is_local) = true; break;
	case '#': set(&&StateFirstChar); break;
	case '{': set(&&StateString); break;
	default:
		undo.push_str(name) = g_strdup(CHR2STR(g_ascii_toupper(chr)));
		goto done;
	}

	return NULL;

StateFirstChar:
	undo.push_str(name) = (gchar *)g_malloc(3);
	name[0] = g_ascii_toupper(chr);
	set(&&StateSecondChar);
	return NULL;

StateSecondChar:
	name[1] = g_ascii_toupper(chr);
	name[2] = '\0';
	goto done;

StateString:
	switch (chr) {
	case '{':
		undo.push_var(nesting)++;
		break;
	case '}':
		if (!nesting)
			goto done;
		undo.push_var(nesting)--;
		break;
	}

	insert = string_machine.input(chr);
	if (!insert)
		return NULL;

	undo.push_str(name);
	String::append(name, insert);
	g_free(insert);
	return NULL;

done:
	QRegisterTable &table = is_local ? *QRegisters::locals
					 : QRegisters::globals;
	QRegister *reg = table[name];

	if (!reg) {
		if (!initialize)
			throw State::InvalidQRegError(name, is_local);
		reg = table.insert(new QRegister(name));
	}

	return reg;
}

/*
 * Command states
 */

StateExpectQReg::StateExpectQReg(bool initialize) : State(), machine(initialize)
{
	transitions['\0'] = this;
}

State *
StateExpectQReg::custom(gchar chr) throw (Error, ReplaceCmdline)
{
	QRegister *reg = machine.input(chr);

	if (!reg)
		return this;
	machine.reset();

	return got_register(*reg);
}

State *
StatePushQReg::got_register(QRegister &reg) throw (Error)
{
	BEGIN_EXEC(&States::start);

	QRegisters::stack.push(reg);

	return &States::start;
}

State *
StatePopQReg::got_register(QRegister &reg) throw (Error)
{
	BEGIN_EXEC(&States::start);

	if (!QRegisters::stack.pop(reg))
		throw Error("Q-Register stack is empty");

	return &States::start;
}

State *
StateEQCommand::got_register(QRegister &reg) throw (Error)
{
	BEGIN_EXEC(&States::loadqreg);
	register_argument = &reg;
	return &States::loadqreg;
}

State *
StateLoadQReg::done(const gchar *str) throw (Error)
{
	BEGIN_EXEC(&States::start);

	if (*str) {
		register_argument->undo_load();
		if (!register_argument->load(str))
			throw Error("Cannot load \"%s\" into Q-Register \"%s\"",
				    str, register_argument->name);
	} else {
		if (ring.current)
			ring.undo_edit();
		else /* QRegisters::current != NULL */
			QRegisters::undo_edit();
		QRegisters::globals.edit(register_argument);
	}

	return &States::start;
}

State *
StateCtlUCommand::got_register(QRegister &reg) throw (Error)
{
	BEGIN_EXEC(&States::setqregstring);
	register_argument = &reg;
	return &States::setqregstring;
}

State *
StateSetQRegString::done(const gchar *str) throw (Error)
{
	BEGIN_EXEC(&States::start);

	register_argument->undo_set_string();
	register_argument->set_string(str);

	return &States::start;
}

State *
StateGetQRegString::got_register(QRegister &reg) throw (Error)
{
	gchar *str;

	BEGIN_EXEC(&States::start);

	str = reg.get_string();
	if (*str) {
		interface.ssm(SCI_BEGINUNDOACTION);
		interface.ssm(SCI_ADDTEXT, strlen(str), (sptr_t)str);
		interface.ssm(SCI_SCROLLCARET);
		interface.ssm(SCI_ENDUNDOACTION);
		ring.dirtify();

		undo.push_msg(SCI_UNDO);
	}
	g_free(str);

	return &States::start;
}

State *
StateGetQRegInteger::got_register(QRegister &reg) throw (Error)
{
	BEGIN_EXEC(&States::start);

	expressions.eval();
	expressions.push(reg.get_integer());

	return &States::start;
}

State *
StateSetQRegInteger::got_register(QRegister &reg) throw (Error)
{
	BEGIN_EXEC(&States::start);

	reg.undo_set_integer();
	reg.set_integer(expressions.pop_num_calc());

	return &States::start;
}

State *
StateIncreaseQReg::got_register(QRegister &reg) throw (Error)
{
	gint64 res;

	BEGIN_EXEC(&States::start);

	reg.undo_set_integer();
	res = reg.get_integer() + expressions.pop_num_calc();
	expressions.push(reg.set_integer(res));

	return &States::start;
}

State *
StateMacro::got_register(QRegister &reg) throw (Error, ReplaceCmdline)
{
	BEGIN_EXEC(&States::start);

	/* don't create new local Q-Registers if colon modifier is given */
	reg.execute(!eval_colon());

	return &States::start;
}

State *
StateCopyToQReg::got_register(QRegister &reg) throw (Error)
{
	gint64 from, len;
	Sci_TextRange tr;

	BEGIN_EXEC(&States::start);
	expressions.eval();

	if (expressions.args() <= 1) {
		from = interface.ssm(SCI_GETCURRENTPOS);
		sptr_t line = interface.ssm(SCI_LINEFROMPOSITION, from) +
			      expressions.pop_num_calc();

		if (!Validate::line(line))
			throw RangeError("X");

		len = interface.ssm(SCI_POSITIONFROMLINE, line) - from;

		if (len < 0) {
			from += len;
			len *= -1;
		}
	} else {
		gint64 to = expressions.pop_num();
		from = expressions.pop_num();

		if (!Validate::pos(from) || !Validate::pos(to))
			throw RangeError("X");

		len = to - from;
	}

	tr.chrg.cpMin = from;
	tr.chrg.cpMax = from + len;
	tr.lpstrText = (char *)g_malloc(len + 1);
	interface.ssm(SCI_GETTEXTRANGE, 0, (sptr_t)&tr);

	if (eval_colon()) {
		reg.undo_append_string();
		reg.append_string(tr.lpstrText);
	} else {
		reg.undo_set_string();
		reg.set_string(tr.lpstrText);
	}
	g_free(tr.lpstrText);

	return &States::start;
}
