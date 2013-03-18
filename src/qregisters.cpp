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
#include "document.h"
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
	StateMacroFile		macro_file;
	StateCopyToQReg		copytoqreg;
}

namespace QRegisters {
	QRegisterTable		*locals = NULL;
	QRegister		*current = NULL;

	void
	undo_edit(void)
	{
		current->update_string();
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
	string.reset();

	interface.ssm(SCI_BEGINUNDOACTION);
	interface.ssm(SCI_SETTEXT, 0, (sptr_t)(str ? : ""));
	interface.ssm(SCI_ENDUNDOACTION);

	current_edit();
}

void
QRegisterData::undo_set_string(void)
{
	/* set_string() assumes that parameters have been saved */
	current_doc_update();

	if (!must_undo)
		return;

	if (ring.current)
		ring.current->undo_edit();
	else if (QRegisters::current)
		QRegisters::current->undo_edit();

	string.undo_reset();
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

	if (!string.is_initialized())
		return g_strdup("");

	current_doc_update();
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
	string.edit();
}

void
QRegisterData::undo_edit(void)
{
	if (must_undo)
		string.undo_edit();
}

void
QRegister::edit(void)
{
	string.edit();
	interface.info_update(this);
}

void
QRegister::undo_edit(void)
{
	if (!must_undo)
		return;

	interface.undo_info_update(this);
	string.undo_edit();
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
	string.reset();

	interface.ssm(SCI_BEGINUNDOACTION);
	interface.ssm(SCI_CLEARALL);
	interface.ssm(SCI_APPENDTEXT, size, (sptr_t)contents);
	interface.ssm(SCI_ENDUNDOACTION);

	g_free(contents);

	current_edit();

	return true;
}

tecoInt
QRegisterBufferInfo::get_integer(void)
{
	tecoInt id = 1;

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
		insert(q);
	for (gchar q = '0'; q <= '9'; q++)
		insert(q);
}

void
QRegisterTable::edit(QRegister *reg)
{
	current_doc_update();
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

	gchar *str = reg.get_string();
	if (*str)
		entry->set_string(str);
	g_free(str);
	entry->string.update(reg.string);
	entry->set_integer(reg.get_integer());

	SLIST_INSERT_HEAD(&head, entry, entries);
	undo.push(new UndoTokenPop(this));
}

bool
QRegisterStack::pop(QRegister &reg)
{
	Entry *entry = SLIST_FIRST(&head);

	if (!entry)
		return false;

	reg.undo_set_integer();
	reg.set_integer(entry->get_integer());

	/* exchange document ownership between Stack entry and Q-Register */
	if (reg.must_undo)
		reg.string.undo_exchange();
	entry->string.undo_exchange();
	entry->string.exchange(reg.string);

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

MICROSTATE_START;
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

	if (mode > MODE_NORMAL)
		return NULL;

	insert = string_machine.input(chr);
	if (!insert)
		return NULL;

	undo.push_str(name);
	String::append(name, insert);
	g_free(insert);
	return NULL;

done:
	if (mode > MODE_NORMAL)
		/*
		 * FIXME: currently we must return *some* register
		 * since got_register() expects one
		 */
		return QRegisters::globals["0"];

	QRegisterTable &table = is_local ? *QRegisters::locals
					 : QRegisters::globals;
	QRegister *reg = table[name];

	if (!reg) {
		if (!initialize)
			throw State::InvalidQRegError(name, is_local);
		reg = table.insert(name);
		table.undo_remove(reg);
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

/*$
 * [q -- Save Q-Register
 *
 * Save Q-Register <q> contents on the global Q-Register push-down
 * stack.
 */
State *
StatePushQReg::got_register(QRegister &reg) throw (Error)
{
	BEGIN_EXEC(&States::start);

	QRegisters::stack.push(reg);

	return &States::start;
}

/*$
 * ]q -- Restore Q-Register
 *
 * Restore Q-Register <q> by replacing its contents
 * with the contents of the register saved on top of
 * the Q-Register push-down stack.
 * The stack entry is popped.
 *
 * In interactive mode, the original contents of <q>
 * are not immediately reclaimed but are kept in memory
 * to support rubbing out the command.
 * Memory is reclaimed on command-line termination.
 */
State *
StatePopQReg::got_register(QRegister &reg) throw (Error)
{
	BEGIN_EXEC(&States::start);

	if (!QRegisters::stack.pop(reg))
		throw Error("Q-Register stack is empty");

	return &States::start;
}

/*$
 * EQq$ -- Edit or load Q-Register
 * EQq[file]$
 *
 * When specified with an empty <file> string argument,
 * EQ makes <q> the currently edited Q-Register.
 * Otherwise, when <file> is specified, it is the
 * name of a file to read into Q-Register <q>.
 * When loading a file, the currently edited
 * buffer/register is not changed and the edit position
 * of register <q> is reset to 0.
 *
 * Undefined Q-Registers will be defined.
 * The command fails if <file> could not be read.
 */
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

/*$
 * ^Uq[string]$ -- Set Q-Register string
 *
 * Sets string-part of Q-Register <q> to <string>.
 * If <q> is undefined, it will be defined.
 *
 * String-building is by default disabled for ^U commands.
 */
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

/*$
 * Gq -- Insert Q-Register string
 *
 * Inserts the string of Q-Register <q> into the buffer
 * at its current position.
 * Specifying an undefined <q> yields an error.
 */
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

/*$
 * Qq -> n -- Query Q-Register integer
 *
 * Gets and returns the integer-part of Q-Register <q>.
 * The command fails for undefined registers.
 */
State *
StateGetQRegInteger::got_register(QRegister &reg) throw (Error)
{
	BEGIN_EXEC(&States::start);

	expressions.eval();
	expressions.push(reg.get_integer());

	return &States::start;
}

/*$
 * [n]Uq -- Set Q-Register integer
 *
 * Sets the integer-part of Q-Register <q> to <n>.
 * If <n> is omitted, the sign prefix is implied.
 *
 * The register is defined if it does not exist.
 */
/** @bug perhaps it's better to imply 0! */
State *
StateSetQRegInteger::got_register(QRegister &reg) throw (Error)
{
	BEGIN_EXEC(&States::start);

	reg.undo_set_integer();
	reg.set_integer(expressions.pop_num_calc());

	return &States::start;
}

/*$
 * [n]%q -> q+n -- Increase Q-Register integer
 *
 * Add <n> to the integer part of register <q>, returning
 * its new value.
 * <q> will be defined if it does not exist.
 */
State *
StateIncreaseQReg::got_register(QRegister &reg) throw (Error)
{
	tecoInt res;

	BEGIN_EXEC(&States::start);

	reg.undo_set_integer();
	res = reg.get_integer() + expressions.pop_num_calc();
	expressions.push(reg.set_integer(res));

	return &States::start;
}

/*$
 * Mq -- Execute macro
 * :Mq
 *
 * Execute macro stored in string of Q-Register <q>.
 * The command itself does not push or pop and arguments from the stack
 * but the macro executed might well do so.
 * The new macro invocation level will contain its own go-to label table
 * and local Q-Register table.
 * Except when the command is colon-modified - in this case, local
 * Q-Registers referenced in the macro refer to the parent macro-level's
 * local Q-Register table (or whatever level defined one last).
 *
 * Errors during the macro execution will propagate to the M command.
 * In other words if a command in the macro fails, the M command will fail
 * and this failure propagates until the top-level macro (e.g.
 * the command-line macro).
 *
 * Note that the string of <q> will be copied upon macro execution,
 * so subsequent changes to Q-Register <q> from inside the macro do
 * not modify the executed code.
 */
State *
StateMacro::got_register(QRegister &reg) throw (Error, ReplaceCmdline)
{
	BEGIN_EXEC(&States::start);

	/* don't create new local Q-Registers if colon modifier is given */
	reg.execute(!eval_colon());

	return &States::start;
}

/*$
 * EMfile$ -- Execute macro from file
 * :EMfile$
 *
 * Read the file with name <file> into memory and execute its contents
 * as a macro.
 * It is otherwise similar to the \(lqM\(rq command.
 *
 * If <file> could not be read, the command yields an error.
 */
State *
StateMacroFile::done(const gchar *str) throw (Error)
{
	BEGIN_EXEC(&States::start);

	/* don't create new local Q-Registers if colon modifier is given */
	if (!Execute::file(str, !eval_colon()))
		throw Error("Cannot execute macro from file \"%s\"", str);

	return &States::start;
}

/*$
 * [lines]Xq -- Copy into or append to Q-Register
 * -Xq
 * from,toXq
 * [lines]:Xq
 * -:Xq
 * from,to:Xq
 *
 * Copy the next or previous number of <lines> from the buffer
 * into the Q-Register <q> string.
 * If <lines> is omitted, the sign prefix is implied.
 * If two arguments are specified, the characters beginning
 * at position <from> up to the character at position <to>
 * are copied.
 * The semantics of the arguments is analogous to the K
 * command's arguments.
 * If the command is colon-modified, the characters will be
 * appended to the end of register <q> instead.
 *
 * Register <q> will be created if it is undefined.
 */
State *
StateCopyToQReg::got_register(QRegister &reg) throw (Error)
{
	tecoInt from, len;
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
		tecoInt to = expressions.pop_num();
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
