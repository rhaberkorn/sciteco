/*
 * Copyright (C) 2012-2015 Robin Haberkorn
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
#include "string-utils.h"
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
	StateEPctCommand	epctcommand;
	StateSaveQReg		saveqreg;
	StateQueryQReg		queryqreg;
	StateCtlUCommand	ctlucommand;
	StateEUCommand		eucommand;
	StateSetQRegString	setqregstring_nobuilding(false);
	StateSetQRegString	setqregstring_building(true);
	StateGetQRegString	getqregstring;
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

gsize
QRegisterData::get_string_size(void)
{
	gsize size;

	if (!string.is_initialized())
		return 0;

	if (QRegisters::current)
		QRegisters::current->string.update(QRegisters::view);

	string.edit(QRegisters::view);

	size = QRegisters::view.ssm(SCI_GETLENGTH);

	if (QRegisters::current)
		QRegisters::current->string.edit(QRegisters::view);

	return size;
}

gint
QRegisterData::get_character(gint position)
{
	gint ret = -1;

	if (position < 0)
		return -1;

	if (QRegisters::current)
		QRegisters::current->string.update(QRegisters::view);

	string.edit(QRegisters::view);

	if (position < QRegisters::view.ssm(SCI_GETLENGTH))
		ret = QRegisters::view.ssm(SCI_GETCHARAT, position);

	if (QRegisters::current)
		QRegisters::current->string.edit(QRegisters::view);

	return ret;
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
	/*
	 * We might be switching the current document
	 * to a buffer.
	 */
	string.update(QRegisters::view);

	if (!must_undo)
		return;

	interface.undo_info_update(this);
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
	undo_set_string();

	if (QRegisters::current)
		QRegisters::current->string.update(QRegisters::view);

	string.edit(QRegisters::view);
	string.reset();

	/*
	 * undo_set_string() pushes undo tokens that restore
	 * the previous document in the view.
	 * So if loading fails, QRegisters::current will be
	 * made the current document again.
	 */
	QRegisters::view.load(filename);

	if (QRegisters::current)
		QRegisters::current->string.edit(QRegisters::view);
}

void
QRegister::save(const gchar *filename)
{
	if (QRegisters::current)
		QRegisters::current->string.update(QRegisters::view);

	string.edit(QRegisters::view);

	try {
		QRegisters::view.save(filename);
	} catch (...) {
		if (QRegisters::current)
			QRegisters::current->string.edit(QRegisters::view);
		throw; /* forward */
	}

	if (QRegisters::current)
		QRegisters::current->string.edit(QRegisters::view);
}

tecoInt
QRegisterBufferInfo::set_integer(tecoInt v)
{
	if (!ring.edit(v))
		throw Error("Invalid buffer id %" TECO_INTEGER_FORMAT, v);

	return v;
}

void
QRegisterBufferInfo::undo_set_integer(void)
{
	current_doc_undo_edit();
}

tecoInt
QRegisterBufferInfo::get_integer(void)
{
	return ring.get_id();
}

gchar *
QRegisterBufferInfo::get_string(void)
{
	return g_strdup(ring.current->filename ? : "");
}

gsize
QRegisterBufferInfo::get_string_size(void)
{
	return ring.current->filename ? strlen(ring.current->filename) : 0;
}

gint
QRegisterBufferInfo::get_character(gint position)
{
	if (position < 0 || position >= (gint)get_string_size())
		return -1;

	return ring.current->filename[position];
}

void
QRegisterBufferInfo::edit(void)
{
	QRegister::edit();

	QRegisters::view.ssm(SCI_BEGINUNDOACTION);
	QRegisters::view.ssm(SCI_SETTEXT, 0,
	                     (sptr_t)(ring.current->filename ? : ""));
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

/*
 * NOTE: by not making this inline,
 * we can access QRegisters::current
 */
void
QRegisterTable::edit(QRegister *reg)
{
	reg->edit();
	QRegisters::current = reg;
}

/*
 * This is similar to RBTree::clear() but
 * has the advantage that we can check whether some
 * register is currently edited.
 * Since this is not a destructor, we can throw
 * errors.
 * Therefore this method should be called before
 * a (local) QRegisterTable is deleted.
 */
void
QRegisterTable::clear(void)
{
	QRegister *cur;

	while ((cur = (QRegister *)min())) {
		if (cur == QRegisters::current)
			throw Error("Currently edited Q-Register \"%s\" "
			            "cannot be discarded", cur->name);

		remove(cur);
		delete cur;
	}
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
	static const gchar *type2name[] = {
		/* [HOOK_ADD-1] = */	"ADD",
		/* [HOOK_EDIT-1] = */	"EDIT",
		/* [HOOK_CLOSE-1] = */	"CLOSE",
		/* [HOOK_QUIT-1] = */	"QUIT",
	};

	QRegister *reg;

	if (!(Flags::ed & Flags::ED_HOOKS))
		return;

	try {
		reg = globals["ED"];
		if (!reg)
			throw Error("Undefined ED-hook register (\"ED\")");

		/*
		 * ED-hook execution should not see any
		 * integer parameters but the hook type.
		 * Such parameters could confuse the ED macro
		 * and macro authors do not expect side effects
		 * of ED macros on the expression stack.
		 * Also make sure it does not leave behind
		 * additional arguments on the stack.
		 *
		 * So this effectively executes:
		 * (typeM[ED]^[)
		 */
		expressions.push(Expressions::OP_BRACE);
		expressions.push(type);
		reg->execute();
		expressions.discard_args();
		expressions.eval(true);
	} catch (Error &error) {
		const gchar *type_str = type2name[type-1];

		error.add_frame(new Error::EDHookFrame(type_str));
		throw; /* forward */
	}
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
		register_argument->load(str);
	} else {
		/* Edit Q-Register */
		current_doc_undo_edit();
		QRegisters::globals.edit(register_argument);
	}

	return &States::start;
}

/*$
 * E%q<file>$ -- Save Q-Register string to file
 *
 * Saves the string contents of Q-Register <q> to
 * <file>.
 * The <file> must always be specified, as Q-Registers
 * have no notion of associated file names.
 *
 * In interactive mode, the E% command may be rubbed out,
 * restoring the previous state of <file>.
 * This follows the same rules as with the \fBEW\fP command.
 *
 * File names may also be tab-completed and string building
 * characters are enabled by default.
 */
State *
StateEPctCommand::got_register(QRegister &reg)
{
	BEGIN_EXEC(&States::saveqreg);
	register_argument = &reg;
	return &States::saveqreg;
}

State *
StateSaveQReg::done(const gchar *str)
{
	BEGIN_EXEC(&States::start);
	register_argument->save(str);
	return &States::start;
}

/*$
 * Qq -> n -- Query Q-Register integer or string
 * <position>Qq -> character
 * :Qq -> size
 *
 * Without any arguments, get and return the integer-part of
 * Q-Register <q>.
 *
 * With one argument, return the <character> code at <position>
 * from the string-part of Q-Register <q>.
 * Positions are handled like buffer positions \(em they
 * begin at 0 up to the length of the string minus 1.
 * An error is thrown for invalid positions.
 *
 * When colon-modified, Q does not pop any arguments from
 * the expression stack and returns the <size> of the string
 * in Q-Register <q>.
 * Naturally, for empty strings, 0 is returned.
 *
 * The command fails for undefined registers.
 */
State *
StateQueryQReg::got_register(QRegister &reg)
{
	BEGIN_EXEC(&States::start);

	expressions.eval();

	if (eval_colon()) {
		/* Query Q-Register string size */
		expressions.push(reg.get_string_size());
	} else if (expressions.args() > 0) {
		/* Query character from Q-Register string */
		gint c = reg.get_character(expressions.pop_num_calc());
		if (c < 0)
			throw RangeError('Q');
		expressions.push(c);
	} else {
		/* Query integer */
		expressions.push(reg.get_integer());
	}

	return &States::start;
}

/*$
 * [c1,c2,...]^Uq[string]$ -- Set or append to Q-Register string without string building
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
 * String-building characters are \fBdisabled\fP for ^U
 * commands.
 * Therefore they are especially well-suited for defining
 * \*(ST macros, since string building characters in the
 * desired Q-Register contents do not have to be escaped.
 * The \fBEU\fP command may be used where string building
 * is desired.
 */
State *
StateCtlUCommand::got_register(QRegister &reg)
{
	BEGIN_EXEC(&States::setqregstring_nobuilding);
	register_argument = &reg;
	return &States::setqregstring_nobuilding;
}

/*$
 * [c1,c2,...]EUq[string]$ -- Set or append to Q-Register string with string building characters
 * [c1,c2,...]:EUq[string]$
 *
 * This command sets or appends to the contents of
 * Q-Register \fIq\fP.
 * It is identical to the \fB^U\fP command, except
 * that this form of the command has string building
 * characters \fBenabled\fP.
 */
State *
StateEUCommand::got_register(QRegister &reg)
{
	BEGIN_EXEC(&States::setqregstring_building);
	register_argument = &reg;
	return &States::setqregstring_building;
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
