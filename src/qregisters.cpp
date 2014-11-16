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
#include "error.h"
#include "qregisters.h"

namespace SciTECO {

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

	static QRegisterStack	stack;
}

static QRegister *register_argument = NULL;

void
QRegisterData::set_string(const gchar *str)
{
	if (QRegisters::current)
		QRegisters::current->string.update(QRegisters::view);

	string.reset();
	string.edit(QRegisters::view);

	QRegisters::view.ssm(SCI_BEGINUNDOACTION);
	QRegisters::view.ssm(SCI_SETTEXT, 0,
	                     (sptr_t)(str ? : ""));
	QRegisters::view.ssm(SCI_ENDUNDOACTION);

	if (QRegisters::current)
		QRegisters::current->string.edit(QRegisters::view);
}

void
QRegisterData::undo_set_string(void)
{
	if (!must_undo)
		return;

	/*
	 * Necessary, so that upon rubout the
	 * string's parameters are restored.
	 */
	string.update(QRegisters::view);

	if (QRegisters::current && QRegisters::current->must_undo)
		QRegisters::current->string.undo_edit(QRegisters::view);

	string.undo_reset();
	QRegisters::view.undo_ssm(SCI_UNDO);

	string.undo_edit(QRegisters::view);
}

void
QRegisterData::append_string(const gchar *str)
{
	/*
	 * NOTE: Will not create undo action
	 * if string is empty.
	 * Also, appending preserves the string's
	 * parameters.
	 */
	if (!str || !*str)
		return;

	if (QRegisters::current)
		QRegisters::current->string.update(QRegisters::view);

	string.edit(QRegisters::view);

	QRegisters::view.ssm(SCI_BEGINUNDOACTION);
	QRegisters::view.ssm(SCI_APPENDTEXT,
	                      strlen(str), (sptr_t)str);
	QRegisters::view.ssm(SCI_ENDUNDOACTION);

	if (QRegisters::current)
		QRegisters::current->string.edit(QRegisters::view);
}

gchar *
QRegisterData::get_string(void)
{
	gint size;
	gchar *str;

	if (!string.is_initialized())
		return g_strdup("");

	if (QRegisters::current)
		QRegisters::current->string.update(QRegisters::view);

	string.edit(QRegisters::view);

	size = QRegisters::view.ssm(SCI_GETLENGTH) + 1;
	str = (gchar *)g_malloc(size);
	QRegisters::view.ssm(SCI_GETTEXT, size, (sptr_t)str);

	if (QRegisters::current)
		QRegisters::current->string.edit(QRegisters::view);

	return str;
}

void
QRegister::edit(void)
{
	if (QRegisters::current)
		QRegisters::current->string.update(QRegisters::view);

	string.edit(QRegisters::view);
	interface.show_view(&QRegisters::view);
	interface.info_update(this);
}

void
QRegister::undo_edit(void)
{
	if (!must_undo)
		return;

	interface.undo_info_update(this);
	string.update(QRegisters::view);
	string.undo_edit(QRegisters::view);
	interface.undo_show_view(&QRegisters::view);
}

void
QRegister::execute(bool locals)
{
	gchar *str = get_string();

	try {
		Execute::macro(str, locals);
	} catch (Error &error) {
		error.add_frame(new Error::QRegFrame(name));

		g_free(str);
		throw; /* forward */
	} catch (...) {
		g_free(str);
		throw; /* forward */
	}

	g_free(str);
}

void
QRegister::load(const gchar *filename)
{
	gchar *contents;
	gsize size;

	GError *gerror = NULL;

	/* FIXME: prevent excessive allocations by reading file into buffer */
	if (!g_file_get_contents(filename, &contents, &size, &gerror))
		throw GlibError(gerror);

	string.edit(QRegisters::view);
	string.reset();

	QRegisters::view.ssm(SCI_BEGINUNDOACTION);
	QRegisters::view.ssm(SCI_CLEARALL);
	QRegisters::view.ssm(SCI_APPENDTEXT, size, (sptr_t)contents);
	QRegisters::view.ssm(SCI_ENDUNDOACTION);

	g_free(contents);

	if (QRegisters::current)
		QRegisters::current->string.edit(QRegisters::view);
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
	return g_strdup(ring.current ? ring.current->filename : "");
}

void
QRegisterBufferInfo::edit(void)
{
	QRegister::edit();

	QRegisters::view.ssm(SCI_BEGINUNDOACTION);
	QRegisters::view.ssm(SCI_SETTEXT, 0,
	                     (sptr_t)(ring.current ? ring.current->filename : ""));
	QRegisters::view.ssm(SCI_ENDUNDOACTION);

	QRegisters::view.undo_ssm(SCI_UNDO);
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
	MicroStateMachine<QRegister *>::reset();
	string_machine.reset();
	undo.push_var(is_local) = false;
	undo.push_var(nesting) = 0;
	undo.push_str(name);
	g_free(name);
	name = NULL;
}

QRegister *
QRegSpecMachine::input(gchar chr)
{
	gchar *insert;

MICROSTATE_START;
	switch (chr) {
	case '.': undo.push_var(is_local) = true; break;
	case '#': set(&&StateFirstChar); break;
	case '[': set(&&StateString); break;
	default:
		undo.push_str(name) = String::chrdup(g_ascii_toupper(chr));
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
	case '[':
		undo.push_var(nesting)++;
		break;
	case ']':
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
			throw InvalidQRegError(name, is_local);
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
StateExpectQReg::custom(gchar chr)
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
StatePushQReg::got_register(QRegister &reg)
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
StatePopQReg::got_register(QRegister &reg)
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
StateEQCommand::got_register(QRegister &reg)
{
	BEGIN_EXEC(&States::loadqreg);
	register_argument = &reg;
	return &States::loadqreg;
}

State *
StateLoadQReg::done(const gchar *str)
{
	BEGIN_EXEC(&States::start);

	if (*str) {
		/* Load file into Q-Register */
		register_argument->undo_load();
		register_argument->load(str);
	} else {
		/* Edit Q-Register */
		current_doc_undo_edit();
		QRegisters::globals.edit(register_argument);
	}

	return &States::start;
}

/*$
 * [c1,c2,...]^Uq[string]$ -- Set or append to Q-Register string
 * [c1,c2,...]:^Uq[string]$
 *
 * If not colon-modified, it first fills the Q-Register <q>
 * with all the values on the expression stack (interpreted as
 * codepoints).
 * It does so in the order of the arguments, i.e.
 * <c1> will be the first character in <q>, <c2> the second, etc.
 * Eventually the <string> argument is appended to the
 * register.
 * Any existing string value in <q> is overwritten by this operation.
 *
 * In the colon-modified form ^U does not overwrite existing
 * contents of <q> but only appends to it.
 *
 * If <q> is undefined, it will be defined.
 *
 * String-building is by default \fBdisabled\fP for ^U commands.
 */
State *
StateCtlUCommand::got_register(QRegister &reg)
{
	BEGIN_EXEC(&States::setqregstring);
	register_argument = &reg;
	return &States::setqregstring;
}

void
StateSetQRegString::initial(void)
{
	int args;

	expressions.eval();
	args = expressions.args();
	text_added = args > 0;
	if (!args)
		return;

	gchar buffer[args+1];

	buffer[args] = '\0';
	while (args--)
		buffer[args] = (gchar)expressions.pop_num_calc();

	if (eval_colon()) {
		/* append to register */
		register_argument->undo_append_string();
		register_argument->append_string(buffer);
	} else {
		/* set register */
		register_argument->undo_set_string();
		register_argument->set_string(buffer);
	}
}

State *
StateSetQRegString::done(const gchar *str)
{
	BEGIN_EXEC(&States::start);

	if (text_added || eval_colon()) {
		/*
		 * Append to register:
		 * Note that append_string() does not create an UNDOACTION
		 * if str == NULL
		 */
		if (str) {
			register_argument->undo_append_string();
			register_argument->append_string(str);
		}
	} else {
		/* set register */
		register_argument->undo_set_string();
		register_argument->set_string(str);
	}

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
StateGetQRegString::got_register(QRegister &reg)
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

		interface.undo_ssm(SCI_UNDO);
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
StateGetQRegInteger::got_register(QRegister &reg)
{
	BEGIN_EXEC(&States::start);

	expressions.eval();
	expressions.push(reg.get_integer());

	return &States::start;
}

/*$
 * nUq -- Set Q-Register integer
 * -Uq
 * [n]:Uq -> Success|Failure
 *
 * Sets the integer-part of Q-Register <q> to <n>.
 * \(lq-U\(rq is equivalent to \(lq-1U\(rq, otherwise
 * the command fails if <n> is missing.
 *
 * If the command is colon-modified, it returns a success
 * boolean if <n> or \(lq-\(rq is given.
 * Otherwise it returns a failure boolean and does not
 * modify <q>.
 *
 * The register is defined if it does not exist.
 */
State *
StateSetQRegInteger::got_register(QRegister &reg)
{
	BEGIN_EXEC(&States::start);

	expressions.eval();
	if (expressions.args() || expressions.num_sign < 0) {
		reg.undo_set_integer();
		reg.set_integer(expressions.pop_num_calc());

		if (eval_colon())
			expressions.push(SUCCESS);
	} else if (eval_colon()) {
		expressions.push(FAILURE);
	} else {
		throw ArgExpectedError('U');
	}

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
StateIncreaseQReg::got_register(QRegister &reg)
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
StateMacro::got_register(QRegister &reg)
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
StateMacroFile::done(const gchar *str)
{
	BEGIN_EXEC(&States::start);

	/* don't create new local Q-Registers if colon modifier is given */
	Execute::file(str, !eval_colon());

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
StateCopyToQReg::got_register(QRegister &reg)
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

} /* namespace SciTECO */
