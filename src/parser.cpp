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

#include <stdarg.h>
#include <string.h>
#include <exception>

#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>

#include "sciteco.h"
#include "interface.h"
#include "undo.h"
#include "expressions.h"
#include "goto.h"
#include "qregisters.h"
#include "ring.h"
#include "parser.h"
#include "symbols.h"
#include "search.h"
#include "spawn.h"
#include "cmdline.h"

//#define DEBUG

gint macro_pc = 0;

namespace States {
	StateStart 		start;
	StateControl		control;
	StateASCII		ascii;
	StateFCommand		fcommand;
	StateCondCommand	condcommand;
	StateECommand		ecommand;
	StateScintilla_symbols	scintilla_symbols;
	StateScintilla_lParam	scintilla_lparam;
	StateInsert		insert;

	State *current = &start;
}

namespace Modifiers {
	static bool colon = false;
	static bool at = false;
}

enum Mode mode = MODE_NORMAL;

/* FIXME: perhaps integrate into Mode */
static bool skip_else = false;

static gint nest_level = 0;

gchar *strings[2] = {NULL, NULL};
gchar escape_char = '\x1B';

/*
 * handles all expected exceptions, converting them to
 * State::Error and preparing them for stack frame insertion
 */
void
Execute::step(const gchar *macro, gint stop_pos)
	     throw (State::Error, ReplaceCmdline)
{
	while (macro_pc < stop_pos) {
#ifdef DEBUG
		g_printf("EXEC(%d): input='%c'/%x, state=%p, mode=%d\n",
			 macro_pc, macro[macro_pc], macro[macro_pc],
			 States::current, mode);
#endif

		try {
			/*
			 * convert bad_alloc and other C++ standard
			 * library exceptions
			 */
			try {
				if (interface.is_interrupted())
					throw State::Error("Interrupted");

				State::input(macro[macro_pc]);
			} catch (std::exception &error) {
				throw State::StdError(error);
			}
		} catch (State::Error &error) {
			error.pos = macro_pc;
			String::get_coord(macro, error.pos,
					  error.line, error.column);
			throw; /* forward */
		}
		macro_pc++;
	}
}

/*
 * may throw non State::Error exceptions which are not to be
 * associated with the macro invocation stack frame
 */
void
Execute::macro(const gchar *macro, bool locals)
{
	GotoTable *parent_goto_table = Goto::table;
	GotoTable macro_goto_table(false);

	QRegisterTable *parent_locals;

	State *parent_state = States::current;
	gint parent_pc = macro_pc;

	/*
	 * need this to fixup state on rubout: state machine emits undo token
	 * resetting state to parent's one, but the macro executed also emitted
	 * undo tokens resetting the state to StateStart
	 */
	undo.push_var(States::current) = &States::start;
	macro_pc = 0;

	Goto::table = &macro_goto_table;
	/* locals are allocated so that we do not waste call stack space */
	if (locals) {
		parent_locals = QRegisters::locals;
		QRegisters::locals = new QRegisterTable(false);
	}

	try {
		step(macro, strlen(macro));
		if (Goto::skip_label) {
			State::Error error("Label \"%s\" not found",
					   Goto::skip_label);
			error.pos = strlen(macro);
			String::get_coord(macro, error.pos,
					  error.line, error.column);
			throw error;
		}
	} catch (...) {
		g_free(Goto::skip_label);
		Goto::skip_label = NULL;

		if (locals) {
			delete QRegisters::locals;
			QRegisters::locals = parent_locals;
		}
		Goto::table = parent_goto_table;

		macro_pc = parent_pc;
		States::current = parent_state;

		throw; /* forward */
	}

	if (locals) {
		delete QRegisters::locals;
		QRegisters::locals = parent_locals;
	}
	Goto::table = parent_goto_table;

	macro_pc = parent_pc;
	States::current = parent_state;
}

void
Execute::file(const gchar *filename, bool locals)
{
	GError *gerror = NULL;
	gchar *macro_str, *p;

	if (!g_file_get_contents(filename, &macro_str, NULL, &gerror))
		throw State::GError(gerror);
	/* only when executing files, ignore Hash-Bang line */
	if (*macro_str == '#')
		p = MAX(strchr(macro_str, '\r'), strchr(macro_str, '\n'))+1;
	else
		p = macro_str;

	try {
		macro(p, locals);
	} catch (State::Error &error) {
		error.pos += p - macro_str;
		if (*macro_str == '#')
			error.line++;
		error.add_frame(new State::Error::FileFrame(filename));

		g_free(macro_str);
		throw; /* forward */
	} catch (...) {
		g_free(macro_str);
		throw; /* forward */
	}

	g_free(macro_str);
}

ReplaceCmdline::ReplaceCmdline()
{
	QRegister *cmdline_reg = QRegisters::globals["\x1B"];

	new_cmdline = cmdline_reg->get_string();
	for (pos = 0; cmdline[pos] && cmdline[pos] == new_cmdline[pos]; pos++);
	pos++;
}

State::Error::Error(const gchar *fmt, ...)
		   : frames(NULL), pos(0), line(0), column(0)
{
	va_list ap;

	va_start(ap, fmt);
	description = g_strdup_vprintf(fmt, ap);
	va_end(ap);
}

State::Error::Error(const Error &inst)
		   : description(g_strdup(inst.description)),
		     pos(inst.pos), line(inst.line), column(inst.column)
{
	/* shallow copy of the frames */
	frames = g_slist_copy(inst.frames);

	for (GSList *cur = frames; cur; cur = g_slist_next(cur)) {
		Frame *frame = (Frame *)cur->data;
		cur->data = frame->copy();
	}
}

void
State::Error::add_frame(Frame *frame)
{
	frame->pos = pos;
	frame->line = line;
	frame->column = column;

	frames = g_slist_prepend(frames, frame);
}

void
State::Error::display_short(void)
{
	interface.msg(Interface::MSG_ERROR,
		      "%s (at %d)", description, pos);
}

void
State::Error::display_full(void)
{
	gint nr = 0;

	interface.msg(Interface::MSG_ERROR, "%s", description);

	frames = g_slist_reverse(frames);
	for (GSList *cur = frames; cur; cur = g_slist_next(cur)) {
		Frame *frame = (Frame *)cur->data;

		frame->display(nr++);
	}
}

State::Error::~Error()
{
	g_free(description);
	for (GSList *cur = frames; cur; cur = g_slist_next(cur))
		delete (Frame *)cur->data;
	g_slist_free(frames);
}

State::State()
{
	for (guint i = 0; i < G_N_ELEMENTS(transitions); i++)
		transitions[i] = NULL;
}

bool
State::eval_colon(void)
{
	if (!Modifiers::colon)
		return false;

	undo.push_var<bool>(Modifiers::colon);
	Modifiers::colon = false;
	return true;
}

void
State::input(gchar chr)
{
	State *state = States::current;

	for (;;) {
		State *next = state->get_next_state(chr);

		if (next == state)
			break;

		state = next;
		chr = '\0';
	}

	if (state != States::current) {
		undo.push_var<State *>(States::current);
		States::current = state;
	}
}

State *
State::get_next_state(gchar chr)
{
	State *next = NULL;
	guint upper = g_ascii_toupper(chr);

	if (upper < G_N_ELEMENTS(transitions))
		next = transitions[upper];
	if (!next)
		next = custom(chr);
	if (!next)
		throw SyntaxError(chr);

	return next;
}

void
StringBuildingMachine::reset(void)
{
	MicroStateMachine<gchar *>::reset();
	undo.push_obj(qregspec_machine) = NULL;
	undo.push_var(mode) = MODE_NORMAL;
	undo.push_var(toctl) = false;
}

gchar *
StringBuildingMachine::input(gchar chr)
{
	QRegister *reg;

	switch (mode) {
	case MODE_UPPER:
		chr = g_ascii_toupper(chr);
		break;
	case MODE_LOWER:
		chr = g_ascii_tolower(chr);
		break;
	default:
		break;
	}

	if (toctl) {
		if (chr != '^')
			chr = CTL_KEY(g_ascii_toupper(chr));
		undo.push_var(toctl) = false;
	} else if (chr == '^') {
		undo.push_var(toctl) = true;
		return NULL;
	}

MICROSTATE_START;
	switch (chr) {
	case CTL_KEY('Q'):
	case CTL_KEY('R'): set(&&StateEscaped); break;
	case CTL_KEY('V'): set(&&StateLower); break;
	case CTL_KEY('W'): set(&&StateUpper); break;
	case CTL_KEY('E'): set(&&StateCtlE); break;
	default:
		goto StateEscaped;
	}

	return NULL;

StateLower:
	set(StateStart);

	if (chr != CTL_KEY('V'))
		return String::chrdup(g_ascii_tolower(chr));

	undo.push_var(mode) = MODE_LOWER;
	return NULL;

StateUpper:
	set(StateStart);

	if (chr != CTL_KEY('W'))
		return String::chrdup(g_ascii_toupper(chr));

	undo.push_var(mode) = MODE_UPPER;
	return NULL;

StateCtlE:
	switch (g_ascii_toupper(chr)) {
	case '\\':
		undo.push_obj(qregspec_machine) = new QRegSpecMachine;
		set(&&StateCtlENum);
		break;
	case 'Q':
		undo.push_obj(qregspec_machine) = new QRegSpecMachine;
		set(&&StateCtlEQ);
		break;
	case 'U':
		undo.push_obj(qregspec_machine) = new QRegSpecMachine;
		set(&&StateCtlEU);
		break;
	default: {
		gchar *ret = (gchar *)g_malloc(3);

		set(StateStart);
		ret[0] = CTL_KEY('E');
		ret[1] = chr;
		ret[2] = '\0';
		return ret;
	}
	}

	return NULL;

StateCtlENum:
	reg = qregspec_machine->input(chr);
	if (!reg)
		return NULL;

	undo.push_obj(qregspec_machine) = NULL;
	set(StateStart);
	return g_strdup(expressions.format(reg->get_integer()));

StateCtlEU:
	reg = qregspec_machine->input(chr);
	if (!reg)
		return NULL;

	undo.push_obj(qregspec_machine) = NULL;
	set(StateStart);
	return String::chrdup((gchar)reg->get_integer());

StateCtlEQ:
	reg = qregspec_machine->input(chr);
	if (!reg)
		return NULL;

	undo.push_obj(qregspec_machine) = NULL;
	set(StateStart);
	return reg->get_string();

StateEscaped:
	set(StateStart);
	return String::chrdup(chr);
}

StringBuildingMachine::~StringBuildingMachine()
{
	if (qregspec_machine)
		delete qregspec_machine;
}

State *
StateExpectString::custom(gchar chr)
{
	gchar *insert;

	if (chr == '\0') {
		BEGIN_EXEC(this);
		initial();
		return this;
	}

	/*
	 * String termination handling
	 */
	if (Modifiers::at) {
		if (last)
			undo.push_var(Modifiers::at) = false;

		switch (escape_char) {
		case '\x1B':
		case '{':
			undo.push_var(escape_char) = g_ascii_toupper(chr);
			return this;
		}
	}

	if (escape_char == '{') {
		switch (chr) {
		case '{':
			undo.push_var(nesting)++;
			break;
		case '}':
			undo.push_var(nesting)--;
			break;
		}
	} else if (g_ascii_toupper(chr) == escape_char) {
		undo.push_var(nesting)--;
	}

	if (!nesting) {
		State *next;
		gchar *string = strings[0];

		undo.push_str(strings[0]) = NULL;
		if (last)
			undo.push_var(escape_char) = '\x1B';
		nesting = 1;

		if (string_building)
			machine.reset();

		next = done(string ? : "");
		g_free(string);
		return next;
	}

	BEGIN_EXEC(this);

	/*
	 * String building characters
	 */
	if (string_building) {
		insert = machine.input(chr);
		if (!insert)
			return this;
	} else {
		insert = String::chrdup(chr);
	}

	/*
	 * String accumulation
	 */
	undo.push_str(strings[0]);
	String::append(strings[0], insert);

	process(strings[0], strlen(insert));
	g_free(insert);
	return this;
}

StateStart::StateStart() : State()
{
	transitions['\0'] = this;
	init(" \f\r\n\v");

	transitions['!'] = &States::label;
	transitions['O'] = &States::gotocmd;
	transitions['^'] = &States::control;
	transitions['F'] = &States::fcommand;
	transitions['"'] = &States::condcommand;
	transitions['E'] = &States::ecommand;
	transitions['I'] = &States::insert;
	transitions['S'] = &States::search;
	transitions['N'] = &States::searchall;

	transitions['['] = &States::pushqreg;
	transitions[']'] = &States::popqreg;
	transitions['G'] = &States::getqregstring;
	transitions['Q'] = &States::getqreginteger;
	transitions['U'] = &States::setqreginteger;
	transitions['%'] = &States::increaseqreg;
	transitions['M'] = &States::macro;
	transitions['X'] = &States::copytoqreg;
}

void
StateStart::insert_integer(tecoInt v)
{
	const gchar *str = expressions.format(v);

	interface.ssm(SCI_BEGINUNDOACTION);
	interface.ssm(SCI_ADDTEXT, strlen(str), (sptr_t)str);
	interface.ssm(SCI_SCROLLCARET);
	interface.ssm(SCI_ENDUNDOACTION);
	ring.dirtify();

	undo.push_msg(SCI_UNDO);
}

tecoInt
StateStart::read_integer(void)
{
	uptr_t pos = interface.ssm(SCI_GETCURRENTPOS);
	gchar c = (gchar)interface.ssm(SCI_GETCHARAT, pos);
	tecoInt v = 0;
	gint sign = 1;

	if (c == '-') {
		pos++;
		sign = -1;
	}

	for (;;) {
		c = g_ascii_toupper((gchar)interface.ssm(SCI_GETCHARAT, pos));
		if (c >= '0' && c <= '0' + MIN(expressions.radix, 10) - 1)
			v = (v*expressions.radix) + (c - '0');
		else if (c >= 'A' &&
			 c <= 'A' + MIN(expressions.radix - 10, 26) - 1)
			v = (v*expressions.radix) + 10 + (c - 'A');
		else
			break;

		pos++;
	}

	return sign * v;
}

tecoBool
StateStart::move_chars(tecoInt n)
{
	sptr_t pos = interface.ssm(SCI_GETCURRENTPOS);

	if (!Validate::pos(pos + n))
		return FAILURE;

	interface.ssm(SCI_GOTOPOS, pos + n);
	undo.push_msg(SCI_GOTOPOS, pos);
	return SUCCESS;
}

tecoBool
StateStart::move_lines(tecoInt n)
{
	sptr_t pos = interface.ssm(SCI_GETCURRENTPOS);
	sptr_t line = interface.ssm(SCI_LINEFROMPOSITION, pos) + n;

	if (!Validate::line(line))
		return FAILURE;

	interface.ssm(SCI_GOTOLINE, line);
	undo.push_msg(SCI_GOTOPOS, pos);
	return SUCCESS;
}

tecoBool
StateStart::delete_words(tecoInt n)
{
	sptr_t pos, size;

	if (!n)
		return SUCCESS;

	pos = interface.ssm(SCI_GETCURRENTPOS);
	size = interface.ssm(SCI_GETLENGTH);
	interface.ssm(SCI_BEGINUNDOACTION);
	/*
	 * FIXME: would be nice to do this with constant amount of
	 * editor messages. E.g. by using custom algorithm accessing
	 * the internal document buffer.
	 */
	if (n > 0) {
		while (n--) {
			sptr_t size = interface.ssm(SCI_GETLENGTH);
			interface.ssm(SCI_DELWORDRIGHTEND);
			if (size == interface.ssm(SCI_GETLENGTH))
				break;
		}
	} else {
		n *= -1;
		while (n--) {
			sptr_t pos = interface.ssm(SCI_GETCURRENTPOS);
			//interface.ssm(SCI_DELWORDLEFTEND);
			interface.ssm(SCI_WORDLEFTEND);
			if (pos == interface.ssm(SCI_GETCURRENTPOS))
				break;
			interface.ssm(SCI_DELWORDRIGHTEND);
		}
	}
	interface.ssm(SCI_ENDUNDOACTION);

	if (n >= 0) {
		if (size != interface.ssm(SCI_GETLENGTH)) {
			interface.ssm(SCI_UNDO);
			interface.ssm(SCI_GOTOPOS, pos);
		}
		return FAILURE;
	}

	undo.push_msg(SCI_GOTOPOS, pos);
	undo.push_msg(SCI_UNDO);
	ring.dirtify();

	return SUCCESS;
}

State *
StateStart::custom(gchar chr)
{
	tecoInt v;
	tecoBool rc;

	/*
	 * <CTRL/x> commands implemented in StateCtrlCmd
	 */
	if (IS_CTL(chr))
		return States::control.get_next_state(CTL_ECHO(chr));

	/*
	 * arithmetics
	 */
	/*$
	 * [n]0|1|2|3|4|5|6|7|8|9 -> n*Radix+X -- Append digit
	 *
	 * Integer constants in \*(ST may be thought of and are
	 * technically sequences of single-digit commands.
	 * These commands take one argument from the stack
	 * (0 is implied), multiply it with the current radix
	 * (2, 8, 10, 16, ...), add the digit's value and
	 * return the resultant integer.
	 *
	 * The command-like semantics of digits may be abused
	 * in macros, for instance to append digits to computed
	 * integers.
	 * It is not an error to append a digit greater than the
	 * current radix - this may be changed in the future.
	 */
	if (g_ascii_isdigit(chr)) {
		BEGIN_EXEC(this);
		expressions.add_digit(chr);
		return this;
	}

	chr = g_ascii_toupper(chr);
	switch (chr) {
	case '/':
		BEGIN_EXEC(this);
		expressions.push_calc(Expressions::OP_DIV);
		break;

	case '*':
		if (!g_strcmp0(cmdline, "*"))
			/* special save last commandline command */
			return &States::save_cmdline;

		BEGIN_EXEC(this);
		expressions.push_calc(Expressions::OP_MUL);
		break;

	case '+':
		BEGIN_EXEC(this);
		expressions.push_calc(Expressions::OP_ADD);
		break;

	case '-':
		BEGIN_EXEC(this);
		if (!expressions.args())
			expressions.set_num_sign(-expressions.num_sign);
		else
			expressions.push_calc(Expressions::OP_SUB);
		break;

	case '&':
		BEGIN_EXEC(this);
		expressions.push_calc(Expressions::OP_AND);
		break;

	case '#':
		BEGIN_EXEC(this);
		expressions.push_calc(Expressions::OP_OR);
		break;

	case '(':
		BEGIN_EXEC(this);
		if (expressions.num_sign < 0) {
			expressions.set_num_sign(1);
			expressions.eval();
			expressions.push(-1);
			expressions.push_calc(Expressions::OP_MUL);
		}
		expressions.push(Expressions::OP_BRACE);
		break;

	case ')':
		BEGIN_EXEC(this);
		expressions.eval(true);
		break;

	case ',':
		BEGIN_EXEC(this);
		expressions.eval();
		expressions.push(Expressions::OP_NEW);
		break;

	/*$
	 * \&. -> dot -- Return buffer position
	 *
	 * \(lq.\(rq pushes onto the stack, the current
	 * position (also called <dot>) of the currently
	 * selected buffer or Q-Register.
	 */
	case '.':
		BEGIN_EXEC(this);
		expressions.eval();
		expressions.push(interface.ssm(SCI_GETCURRENTPOS));
		break;

	/*$
	 * Z -> size -- Return buffer size
	 *
	 * Pushes onto the stack, the size of the currently selected
	 * buffer or Q-Register.
	 * This is value is also the buffer position of the document's
	 * end.
	 */
	case 'Z':
		BEGIN_EXEC(this);
		expressions.eval();
		expressions.push(interface.ssm(SCI_GETLENGTH));
		break;

	/*$
	 * H -> 0,Z -- Return range for entire buffer
	 *
	 * Pushes onto the stack the integer 0 (position of buffer
	 * beginning) and the current buffer's size.
	 * It is thus often equivalent to the expression
	 * \(lq0,Z\(rq, or more generally \(lq(0,Z)\(rq.
	 */
	case 'H':
		BEGIN_EXEC(this);
		expressions.eval();
		expressions.push(0);
		expressions.push(interface.ssm(SCI_GETLENGTH));
		break;

	/*$
	 * n\\ -- Insert or read ASCII numbers
	 * \\ -> n
	 *
	 * Backslash pops a value from the stack, formats it
	 * according to the current radix and inserts it in the
	 * current buffer or Q-Register at dot.
	 * If <n> is omitted (empty stack), it does the reverse -
	 * it reads from the current buffer position an integer
	 * in the current radix and pushes it onto the stack.
	 * Dot is not changed when reading integers.
	 *
	 * In other words, the command serializes or deserializes
	 * integers as ASCII characters.
	 */
	case '\\':
		BEGIN_EXEC(this);
		expressions.eval();
		if (expressions.args())
			insert_integer(expressions.pop_num_calc());
		else
			expressions.push(read_integer());
		break;

	/*
	 * control structures (loops)
	 */
	case '<':
		if (mode == MODE_PARSE_ONLY_LOOP) {
			undo.push_var<gint>(nest_level);
			nest_level++;
			return this;
		}
		BEGIN_EXEC(this);

		expressions.eval();
		if (!expressions.args())
			/* infinite loop */
			expressions.push(-1);

		if (!expressions.peek_num()) {
			expressions.pop_num();

			/* skip to end of loop */
			undo.push_var<Mode>(mode);
			mode = MODE_PARSE_ONLY_LOOP;
		} else {
			expressions.push(macro_pc);
			expressions.push(Expressions::OP_LOOP);
		}
		break;

	case '>':
		if (mode == MODE_PARSE_ONLY_LOOP) {
			if (!nest_level) {
				undo.push_var<Mode>(mode);
				mode = MODE_NORMAL;
			} else {
				undo.push_var<gint>(nest_level);
				nest_level--;
			}
		} else {
			BEGIN_EXEC(this);
			tecoInt loop_pc, loop_cnt;

			expressions.discard_args();
			g_assert(expressions.pop_op() == Expressions::OP_LOOP);
			loop_pc = expressions.pop_num();
			loop_cnt = expressions.pop_num();

			if (loop_cnt != 1) {
				/* repeat loop */
				macro_pc = loop_pc;
				expressions.push(MAX(loop_cnt - 1, -1));
				expressions.push(loop_pc);
				expressions.push(Expressions::OP_LOOP);
			}
		}
		break;

	/*$
	 * [bool]; -- Conditionally break from loop
	 * [bool]:;
	 *
	 * Breaks from the current inner-most loop if <bool>
	 * signifies failure (non-negative value).
	 * If colon-modified, breaks from the loop if <bool>
	 * signifies success (negative value).
	 *
	 * If the condition code cannot be popped from the stack,
	 * the global search register's condition integer
	 * is implied instead.
	 * This way, you may break on search success/failures
	 * without colon-modifying the search command (or at a
	 * later point).
	 */
	case ';':
		BEGIN_EXEC(this);

		v = QRegisters::globals["_"]->get_integer();
		rc = expressions.pop_num_calc(1, v);
		if (eval_colon())
			rc = ~rc;

		if (IS_FAILURE(rc)) {
			expressions.discard_args();
			if (expressions.pop_op() != Expressions::OP_LOOP)
				/* FIXME: what does standard teco say to this */
				throw Error("Cannot break from loop without loop");
			expressions.pop_num(); /* pc */
			expressions.pop_num(); /* counter */

			/* skip to end of loop */
			undo.push_var<Mode>(mode);
			mode = MODE_PARSE_ONLY_LOOP;
		}
		break;

	/*
	 * control structures (conditionals)
	 */
	case '|':
		if (mode == MODE_PARSE_ONLY_COND) {
			if (!skip_else && !nest_level) {
				undo.push_var<Mode>(mode);
				mode = MODE_NORMAL;
			}
			return this;
		}
		BEGIN_EXEC(this);

		/* skip to end of conditional; skip ELSE-part */
		undo.push_var<Mode>(mode);
		mode = MODE_PARSE_ONLY_COND;
		break;

	case '\'':
		if (mode != MODE_PARSE_ONLY_COND)
			break;

		if (!nest_level) {
			undo.push_var<Mode>(mode);
			mode = MODE_NORMAL;
			undo.push_var<bool>(skip_else);
			skip_else = false;
		} else {
			undo.push_var<gint>(nest_level);
			nest_level--;
		}
		break;

	/*
	 * Command-line editing
	 */
	/*$
	 * { -- Edit command line
	 * }
	 *
	 * The opening curly bracket is a powerful command
	 * to edit command lines but has very simple semantics.
	 * It copies the current commandline into the global
	 * command line editing register (called Escape, i.e.
	 * ASCII 27) and edits this register.
	 * The curly bracket itself is not copied.
	 *
	 * The command line may then be edited using any
	 * \*(ST command or construct.
	 * You may switch between the command line editing
	 * register and other registers or buffers.
	 * The user will then usually reapply (called update)
	 * the current command-line.
	 *
	 * The closing curly bracket will update the current
	 * command-line with the contents of the global command
	 * line editing register.
	 * To do so it merely rubs-out the current command-line
	 * up to the first changed character and inserts
	 * all characters following from the updated command
	 * line into the command stream.
	 * To prevent the undesired rubout of the entire
	 * command-line, the replacement command ("}") is only
	 * allowed when the replacement register currently edited
	 * since it will otherwise be usually empty.
	 *
	 * .B Note:
	 *   - Command line editing only works on command lines,
	 *     but not arbitrary macros.
	 *     It is therefore not available in batch mode and
	 *     will yield an error if used.
	 *   - Command line editing commands may be safely used
	 *     from macro invocations.
	 *     Such macros are called command line editing macros.
	 *   - A command line update from a macro invocation will
	 *     always yield to the outer-most macro level (i.e.
	 *     the command line macro).
	 *     Code following the update command in the macro
	 *     will thus never be executed.
	 *   - As a safe-guard against command line trashing due
	 *     to erroneous changes at the beginning of command
	 *     lines, a backup mechanism is implemented:
	 *     If the updated command line yields an error at
	 *     any command during the update, the original
	 *     command line will be restored with an algorithm
	 *     similar to command line updating and the update
	 *     command will fail instead.
	 *     That way it behaves like any other command that
	 *     yields an error:
	 *     The character resulting in the update is rejected
	 *     by the command line input subsystem.
	 *   - In the rare case that an aforementioned command line
	 *     backup fails, the commands following the erroneous
	 *     character will not be inserted again (will be lost).
	 */
	case '{':
		BEGIN_EXEC(this);
		if (!undo.enabled)
			throw Error("Command-line editing only possible in "
				    "interactive mode");

		if (ring.current)
			ring.undo_edit();
		else /* QRegisters::current != NULL */
			QRegisters::undo_edit();
		QRegisters::globals.edit("\x1B");

		interface.ssm(SCI_BEGINUNDOACTION);
		interface.ssm(SCI_CLEARALL);
		interface.ssm(SCI_ADDTEXT, cmdline_pos-1, (sptr_t)cmdline);
		/* FIXME: scroll into view */
		interface.ssm(SCI_ENDUNDOACTION);

		undo.push_msg(SCI_UNDO);
		break;

	case '}':
		BEGIN_EXEC(this);
		if (!undo.enabled)
			throw Error("Command-line editing only possible in "
				    "interactive mode");
		if (QRegisters::current != QRegisters::globals["\x1B"])
			throw Error("Command-line replacement only allowed when "
				    "editing the replacement register");

		/* replace cmdline in the outer macro environment */
		throw ReplaceCmdline();

	/*
	 * modifiers
	 */
	case '@':
		/*
		 * @ modifier has syntactic significance, so set it even
		 * in PARSE_ONLY* modes
		 */
		undo.push_var<bool>(Modifiers::at);
		Modifiers::at = true;
		break;

	case ':':
		BEGIN_EXEC(this);
		undo.push_var<bool>(Modifiers::colon);
		Modifiers::colon = true;
		break;

	/*
	 * commands
	 */
	/*$
	 * [position]J -- Go to position in buffer
	 * [position]:J -> Success|Failure
	 *
	 * Sets dot to <position>.
	 * If <position> is omitted, 0 is implied and \(lqJ\(rq will
	 * go to the beginning of the buffer.
	 *
	 * If <position> is outside the range of the buffer, the
	 * command yields an error.
	 * If colon-modified, the command will instead return a
	 * condition boolean signalling whether the position could
	 * be changed or not.
	 */
	case 'J':
		BEGIN_EXEC(this);
		v = expressions.pop_num_calc(1, 0);
		if (Validate::pos(v)) {
			undo.push_msg(SCI_GOTOPOS,
				      interface.ssm(SCI_GETCURRENTPOS));
			interface.ssm(SCI_GOTOPOS, v);

			if (eval_colon())
				expressions.push(SUCCESS);
		} else if (eval_colon()) {
			expressions.push(FAILURE);
		} else {
			throw MoveError("J");
		}
		break;

	/*$
	 * [n]C -- Move dot <n> characters
	 * -C
	 * [n]:C -> Success|Failure
	 *
	 * Adds <n> to dot. 1 or -1 is implied if <n> is omitted.
	 * Fails if <n> would move dot off-page.
	 * The colon modifier results in a success-boolean being
	 * returned instead.
	 */
	case 'C':
		BEGIN_EXEC(this);
		rc = move_chars(expressions.pop_num_calc());
		if (eval_colon())
			expressions.push(rc);
		else if (IS_FAILURE(rc))
			throw MoveError("C");
		break;

	/*$
	 * [n]R -- Move dot <n> characters backwards
	 * -R
	 * [n]:R -> Success|Failure
	 *
	 * Subtracts <n> from dot.
	 * It is equivalent to \(lq-nC\(rq.
	 */
	case 'R':
		BEGIN_EXEC(this);
		rc = move_chars(-expressions.pop_num_calc());
		if (eval_colon())
			expressions.push(rc);
		else if (IS_FAILURE(rc))
			throw MoveError("R");
		break;

	/*$
	 * [n]L -- Move dot <n> lines forwards
	 * -L
	 * [n]:L -> Success|Failure
	 *
	 * Move dot to the beginning of the line specified
	 * relatively to the current line.
	 * Therefore a value of 0 for <n> goes to the
	 * beginning of the current line, 1 will go to the
	 * next line, -1 to the previous line etc.
	 * If <n> is omitted, 1 or -1 is implied depending on
	 * the sign prefix.
	 *
	 * If <n> would move dot off-page, the command yields
	 * an error.
	 * The colon-modifer results in a condition boolean
	 * being returned instead.
	 */
	case 'L':
		BEGIN_EXEC(this);
		rc = move_lines(expressions.pop_num_calc());
		if (eval_colon())
			expressions.push(rc);
		else if (IS_FAILURE(rc))
			throw MoveError("L");
		break;

	/*$
	 * [n]B -- Move dot <n> lines backwards
	 * -B
	 * [n]:B -> Success|Failure
	 *
	 * Move dot to the beginning of the line <n>
	 * lines before the current one.
	 * It is equivalent to \(lq-nL\(rq.
	 */
	case 'B':
		BEGIN_EXEC(this);
		rc = move_lines(-expressions.pop_num_calc());
		if (eval_colon())
			expressions.push(rc);
		else if (IS_FAILURE(rc))
			throw MoveError("B");
		break;

	/*$
	 * [n]W -- Move dot by words
	 * -W
	 * [n]:W -> Success|Failure
	 *
	 * Move dot <n> words forward.
	 *   - If <n> is positive, dot is positioned at the beginning
	 *     of the word <n> words after the current one.
	 *   - If <n> is negative, dot is positioned at the end
	 *     of the word <n> words before the current one.
	 *   - If <n> is zero, dot is not moved.
	 *
	 * \(lqW\(rq uses Scintilla's definition of a word as
	 * configurable using the
	 * .B SCI_SETWORDCHARS
	 * message.
	 *
	 * Otherwise, the command's behaviour is analogous to
	 * the \(lqC\(rq command.
	 */
	case 'W': {
		sptr_t pos;
		unsigned int msg = SCI_WORDRIGHTEND;

		BEGIN_EXEC(this);
		v = expressions.pop_num_calc();

		pos = interface.ssm(SCI_GETCURRENTPOS);
		/*
		 * FIXME: would be nice to do this with constant amount of
		 * editor messages. E.g. by using custom algorithm accessing
		 * the internal document buffer.
		 */
		if (v < 0) {
			v *= -1;
			msg = SCI_WORDLEFTEND;
		}
		while (v--) {
			sptr_t pos = interface.ssm(SCI_GETCURRENTPOS);
			interface.ssm(msg);
			if (pos == interface.ssm(SCI_GETCURRENTPOS))
				break;
		}
		if (v < 0) {
			undo.push_msg(SCI_GOTOPOS, pos);
			if (eval_colon())
				expressions.push(SUCCESS);
		} else {
			interface.ssm(SCI_GOTOPOS, pos);
			if (eval_colon())
				expressions.push(FAILURE);
			else
				throw MoveError("W");
		}
		break;
	}

	/*$
	 * [n]V -- Delete words forward
	 * -V
	 * [n]:V -> Success|Failure
	 *
	 * Deletes the next <n> words until the end of the
	 * n'th word after the current one.
	 * If <n> is negative, deletes up to end of the
	 * n'th word before the current one.
	 * If <n> is omitted, 1 or -1 is implied depending on the
	 * sign prefix.
	 *
	 * It uses Scintilla's definition of a word as configurable
	 * using the
	 * .B SCI_SETWORDCHARS
	 * message.
	 *
	 * If the words to delete extend beyond the range of the
	 * buffer, the command yields an error.
	 * If colon-modified it instead returns a condition code.
	 */
	case 'V':
		BEGIN_EXEC(this);
		rc = delete_words(expressions.pop_num_calc());
		if (eval_colon())
			expressions.push(rc);
		else if (IS_FAILURE(rc))
			throw Error("Not enough words to delete with <V>");
		break;

	/*$
	 * [n]Y -- Delete word backwards
	 * -Y
	 * [n]:Y -> Success|Failure
	 *
	 * Delete <n> words backward.
	 * <n>Y is equivalent to \(lq-nV\(rq.
	 */
	case 'Y':
		BEGIN_EXEC(this);
		rc = delete_words(-expressions.pop_num_calc());
		if (eval_colon())
			expressions.push(rc);
		else if (IS_FAILURE(rc))
			throw Error("Not enough words to delete with <Y>");
		break;

	/*$
	 * <n>= -- Show value as message
	 *
	 * Shows integer <n> as a message in the message line and/or
	 * on the console.
	 * It is currently always formatted as a decimal integer and
	 * shown with the user-message severity.
	 */
	/**
	 * @bug makes no sense to imply the sign-prefix!
	 * @todo perhaps care about current radix
	 * @todo colon-modifier to suppress line-break on console?
	 */
	case '=':
		BEGIN_EXEC(this);
		interface.msg(Interface::MSG_USER, "%" TECO_INTEGER_FORMAT,
			      expressions.pop_num_calc());
		break;

	/*$
	 * [n]K -- Kill lines
	 * -K
	 * from,to K
	 * [n]:K -> Success|Failure
	 * from,to:K -> Success|Failure
	 *
	 * Deletes characters up to the beginning of the
	 * line <n> lines after or before the current one.
	 * If <n> is 0, \(lqK\(rq will delete up to the beginning
	 * of the current line.
	 * If <n> is omitted, the sign prefix will be implied.
	 * So to delete the entire line regardless of the position
	 * in it, one can use \(lq0KK\(rq.
	 *
	 * If the deletion is beyond the buffer's range, the command
	 * will yield an error unless it has been colon-modified
	 * so it returns a condition code.
	 *
	 * If two arguments <from> and <to> are available, the
	 * command is synonymous to <from>,<to>D.
	 */
	case 'K':
	/*$
	 * [n]D -- Delete characters
	 * -D
	 * from,to D
	 * [n]:D -> Success|Failure
	 * from,to:D -> Success|Failure
	 *
	 * If <n> is positive, the next <n> characters (up to and
	 * character .+<n>) are deleted.
	 * If <n> is negative, the previous <n> characters are
	 * deleted.
	 * If <n> is omitted, the sign prefix will be implied.
	 *
	 * If two arguments can be popped from the stack, the
	 * command will delete the characters with absolute
	 * position <from> up to <to> from the current buffer.
	 *
	 * If the character range to delete is beyond the buffer's
	 * range, the command will yield an error unless it has
	 * been colon-modified so it returns a condition code
	 * instead.
	 */
	case 'D': {
		tecoInt from, len;

		BEGIN_EXEC(this);
		expressions.eval();

		if (expressions.args() <= 1) {
			from = interface.ssm(SCI_GETCURRENTPOS);
			if (chr == 'D') {
				len = expressions.pop_num_calc();
				rc = TECO_BOOL(Validate::pos(from + len));
			} else /* chr == 'K' */ {
				sptr_t line;
				line = interface.ssm(SCI_LINEFROMPOSITION, from) +
				       expressions.pop_num_calc();
				len = interface.ssm(SCI_POSITIONFROMLINE, line)
				    - from;
				rc = TECO_BOOL(Validate::line(line));
			}
			if (len < 0) {
				len *= -1;
				from -= len;
			}
		} else {
			tecoInt to = expressions.pop_num();
			from = expressions.pop_num();
			len = to - from;
			rc = TECO_BOOL(len >= 0 && Validate::pos(from) &&
				       Validate::pos(to));
		}

		if (eval_colon())
			expressions.push(rc);
		else if (IS_FAILURE(rc))
			throw RangeError(chr);

		if (len == 0 || IS_FAILURE(rc))
			break;

		undo.push_msg(SCI_GOTOPOS, interface.ssm(SCI_GETCURRENTPOS));
		undo.push_msg(SCI_UNDO);

		interface.ssm(SCI_BEGINUNDOACTION);
		interface.ssm(SCI_DELETERANGE, from, len);
		interface.ssm(SCI_ENDUNDOACTION);
		ring.dirtify();
		break;
	}

	/*$
	 * [n]A -> code -- Get character code from buffer
	 * -A -> code
	 *
	 * Returns the character <code> of the character
	 * <n> relative to dot from the buffer.
	 * This can be an ASCII <code> or Unicode codepoint
	 * depending on Scintilla's encoding of the current
	 * buffer.
	 *   - If <n> is 0, return the <code> of the character
	 *     pointed to by dot.
	 *   - If <n> is 1, return the <code> of the character
	 *     immediately after dot.
	 *   - If <n> is -1, return the <code> of the character
	 *     immediately preceding dot, ecetera.
	 *   - If <n> is omitted, the sign prefix is implied.
	 *
	 * If the position of the queried character is off-page,
	 * the command will yield an error.
	 */
	/** @todo does Scintilla really return code points??? */
	case 'A':
		BEGIN_EXEC(this);
		v = interface.ssm(SCI_GETCURRENTPOS) +
		    expressions.pop_num_calc();
		if (!Validate::pos(v))
			throw RangeError("A");
		expressions.push(interface.ssm(SCI_GETCHARAT, v));
		break;

	default:
		throw SyntaxError(chr);
	}

	return this;
}

StateFCommand::StateFCommand() : State()
{
	transitions['\0'] = this;
	transitions['K'] = &States::searchkill;
	transitions['D'] = &States::searchdelete;
	transitions['S'] = &States::replace;
	transitions['R'] = &States::replacedefault;
}

State *
StateFCommand::custom(gchar chr)
{
	switch (chr) {
	/*
	 * loop flow control
	 */
	/*$
	 * F< -- Go to loop start
	 *
	 * Immediately jumps to the current loop's start.
	 * Also works from inside conditionals.
	 */
	case '<':
		BEGIN_EXEC(&States::start);
		/* FIXME: what if in brackets? */
		expressions.discard_args();
		if (expressions.peek_op() == Expressions::OP_LOOP)
			/* repeat loop */
			macro_pc = expressions.peek_num();
		else
			macro_pc = -1;
		break;

	/*$
	 * F> -- Go to loop end
	 *
	 * Jumps to the current loop's end.
	 * If the loop has a counter or runs idefinitely, the jump
	 * is performed immediately.
	 * If the loop has reached its last iteration, parsing
	 * until the loop end command has been found is performed.
	 *
	 * In interactive mode, if the loop is incomplete and must
	 * be exited, you can type in the loop's remaining commands
	 * without them being executed (but they are parsed).
	 */
	case '>': {
		tecoInt loop_pc, loop_cnt;

		BEGIN_EXEC(&States::start);
		/* FIXME: what if in brackets? */
		expressions.discard_args();
		g_assert(expressions.pop_op() == Expressions::OP_LOOP);
		loop_pc = expressions.pop_num();
		loop_cnt = expressions.pop_num();

		if (loop_cnt != 1) {
			/* repeat loop */
			macro_pc = loop_pc;
			expressions.push(MAX(loop_cnt - 1, -1));
			expressions.push(loop_pc);
			expressions.push(Expressions::OP_LOOP);
		} else {
			/* skip to end of loop */
			undo.push_var<Mode>(mode);
			mode = MODE_PARSE_ONLY_LOOP;
		}
		break;
	}

	/*
	 * conditional flow control
	 */
	/*$
	 * F\' -- Jump to end of conditional
	 */
	case '\'':
		BEGIN_EXEC(&States::start);
		/* skip to end of conditional */
		undo.push_var<Mode>(mode);
		mode = MODE_PARSE_ONLY_COND;
		undo.push_var<bool>(skip_else);
		skip_else = true;
		break;

	/*$
	 * F| -- Jump to else-part of conditional
	 *
	 * Jump to else-part of conditional or end of
	 * conditional (only if invoked from inside the
	 * condition's else-part).
	 */
	case '|':
		BEGIN_EXEC(&States::start);
		/* skip to ELSE-part or end of conditional */
		undo.push_var<Mode>(mode);
		mode = MODE_PARSE_ONLY_COND;
		break;

	default:
		throw SyntaxError(chr);
	}

	return &States::start;
}

StateCondCommand::StateCondCommand() : State()
{
	transitions['\0'] = this;
}

State *
StateCondCommand::custom(gchar chr)
{
	tecoInt value = 0;
	bool result;

	switch (mode) {
	case MODE_PARSE_ONLY_COND:
		undo.push_var<gint>(nest_level);
		nest_level++;
		break;
	case MODE_NORMAL:
		value = expressions.pop_num_calc();
		break;
	default:
		break;
	}

	switch (g_ascii_toupper(chr)) {
	case 'A':
		BEGIN_EXEC(&States::start);
		result = g_ascii_isalpha((gchar)value);
		break;
	case 'C':
		BEGIN_EXEC(&States::start);
		result = g_ascii_isalnum((gchar)value) ||
			 value == '.' || value == '$' || value == '_';
		break;
	case 'D':
		BEGIN_EXEC(&States::start);
		result = g_ascii_isdigit((gchar)value);
		break;
	case 'S':
	case 'T':
		BEGIN_EXEC(&States::start);
		result = IS_SUCCESS(value);
		break;
	case 'F':
	case 'U':
		BEGIN_EXEC(&States::start);
		result = IS_FAILURE(value);
		break;
	case 'E':
	case '=':
		BEGIN_EXEC(&States::start);
		result = value == 0;
		break;
	case 'G':
	case '>':
		BEGIN_EXEC(&States::start);
		result = value > 0;
		break;
	case 'L':
	case '<':
		BEGIN_EXEC(&States::start);
		result = value < 0;
		break;
	case 'N':
		BEGIN_EXEC(&States::start);
		result = value != 0;
		break;
	case 'R':
		BEGIN_EXEC(&States::start);
		result = g_ascii_isalnum((gchar)value);
		break;
	case 'V':
		BEGIN_EXEC(&States::start);
		result = g_ascii_islower((gchar)value);
		break;
	case 'W':
		BEGIN_EXEC(&States::start);
		result = g_ascii_isupper((gchar)value);
		break;
	default:
		throw Error("Invalid conditional type \"%c\"", chr);
	}

	if (!result) {
		/* skip to ELSE-part or end of conditional */
		undo.push_var<Mode>(mode);
		mode = MODE_PARSE_ONLY_COND;
	}

	return &States::start;
}

StateControl::StateControl() : State()
{
	transitions['\0'] = this;
	transitions['U'] = &States::ctlucommand;
	transitions['^'] = &States::ascii;
}

State *
StateControl::custom(gchar chr)
{
	switch (g_ascii_toupper(chr)) {
	/*$
	 * ^O -- Set radix to 8 (octal)
	 */
	case 'O':
		BEGIN_EXEC(&States::start);
		expressions.set_radix(8);
		break;

	/*$
	 * ^D -- Set radix to 10 (decimal)
	 */
	case 'D':
		BEGIN_EXEC(&States::start);
		expressions.set_radix(10);
		break;

	/*$
	 * radix^R -- Set and get radix
	 * ^R -> radix
	 *
	 * Set current radix to arbitrary value <radix>.
	 * If <radix> is omitted, the command instead
	 * returns the current radix.
	 */
	case 'R':
		BEGIN_EXEC(&States::start);
		expressions.eval();
		if (!expressions.args())
			expressions.push(expressions.radix);
		else
			expressions.set_radix(expressions.pop_num_calc());
		break;

	/*
	 * Alternatives: ^i, ^I, <CTRL/I>, <TAB>
	 */
	/*$
	 * [char,...]^I[text]$ -- Insert with leading TAB
	 *
	 * ^I (usually typed using the Tab key), is equivalent
	 * to \(lq[char,...],9I[text]$\(rq.
	 * In other words after all the chars on the stack have
	 * been inserted into the buffer, a Tab-character is inserted
	 * and then the optional <text> is inserted interactively.
	 */
	case 'I':
		BEGIN_EXEC(&States::insert);
		expressions.eval();
		expressions.push('\t');
		return &States::insert;

	/*
	 * Alternatives: ^[, <CTRL/[>, <ESC>
	 */
	/*$
	 * ^[ -- Discard all arguments
	 * $
	 *
	 * Pops and discards all values from the stack that
	 * might otherwise be used as arguments to following
	 * commands.
	 * Therefore it stops popping on stack boundaries like
	 * they are introduced by arithmetic brackets or loops.
	 *
	 * Note that ^[ is usually typed using the Escape key.
	 * CTRL+[ however is possible as well and equivalent to
	 * Escape in every manner.
	 * The Caret-[ notation however is processed like any
	 * ordinary command and only works as the discard-arguments
	 * command.
	 */
	case '[':
		BEGIN_EXEC(&States::start);
		expressions.discard_args();
		break;

	/*
	 * Additional numeric operations
	 */
	/*$
	 * n^_ -> ~n -- Binary negation
	 *
	 * Binary negates (complements) <n> and returns
	 * the result.
	 * Binary complements are often used to negate
	 * \*(ST booleans.
	 */
	case '_':
		BEGIN_EXEC(&States::start);
		expressions.push(~expressions.pop_num_calc());
		break;

	case '*':
		BEGIN_EXEC(&States::start);
		expressions.push_calc(Expressions::OP_POW);
		break;

	case '/':
		BEGIN_EXEC(&States::start);
		expressions.push_calc(Expressions::OP_MOD);
		break;

	default:
		throw Error("Unsupported command <^%c>", chr);
	}

	return &States::start;
}

/*$
 * ^^c -> n -- Get ASCII code of character
 *
 * Returns the ASCII code of the character <c>
 * that is part of the command.
 * Can be used in place of integer constants for improved
 * readability.
 * For instance ^^A will return 65.
 *
 * Note that this command can be typed CTRL+Caret or
 * Caret-Caret.
 */
StateASCII::StateASCII() : State()
{
	transitions['\0'] = this;
}

State *
StateASCII::custom(gchar chr)
{
	BEGIN_EXEC(&States::start);

	expressions.push(chr);

	return &States::start;
}

StateECommand::StateECommand() : State()
{
	transitions['\0'] = this;
	transitions['B'] = &States::editfile;
	transitions['C'] = &States::executecommand;
	transitions['G'] = &States::egcommand;
	transitions['M'] = &States::macro_file;
	transitions['S'] = &States::scintilla_symbols;
	transitions['Q'] = &States::eqcommand;
	transitions['W'] = &States::savefile;
}

State *
StateECommand::custom(gchar chr)
{
	switch (g_ascii_toupper(chr)) {
	/*$
	 * [bool]EF -- Remove buffer from ring
	 * -EF
	 *
	 * Removes buffer from buffer ring, effectively
	 * closing it.
	 * If the buffer is dirty (modified), EF will yield
	 * an error.
	 * <bool> may be a specified to enforce closing dirty
	 * buffers.
	 * If it is a Failure condition boolean (negative),
	 * the buffer will be closed unconditionally.
	 * If <bool> is absent, the sign prefix (1 or -1) will
	 * be implied, so \(lq-EF\(rq will always close the buffer.
	 *
	 * It is noteworthy that EF will be executed immediately in
	 * interactive mode but can be rubbed out at a later time
	 * to reopen the file.
	 * Closed files are kept in memory until the command line
	 * is terminated.
	 */
	case 'F':
		BEGIN_EXEC(&States::start);
		if (!ring.current)
			throw Error("No buffer selected");

		if (IS_FAILURE(expressions.pop_num_calc()) &&
		    ring.current->dirty)
			throw Error("Buffer \"%s\" is dirty",
				    ring.current->filename ? : "(Unnamed)");

		ring.close();
		break;

	/*$
	 * flags ED -- Set and get ED-flags
	 * [off,]on ED
	 * ED -> flags
	 *
	 * With arguments, the command will set the ED flags.
	 * <flags> is a bitmap of flags to set.
	 * Specifying one argument to set the flags is a special
	 * case of specifying two arguments that allow to control
	 * which flags to enable/disable.
	 * <off> is a bitmap of flags to disable (set to 0 in ED
	 * flags) and <on> is a bitmap of flags that is ORed into
	 * the flags variable.
	 * If <off> is omitted, the value 0^_ is implied.
	 * In otherwords, all flags are turned off before turning
	 * on the <on> flags.
	 * Without any argument ED returns the current flags.
	 *
	 * Currently, the following flags are used by \*(ST:
	 *   - 32: Enable/Disable executing register \(lq0\(rq hooks
	 *   - 64: Enable/Disable function key macros
	 *   - 128: Enable/Disable enforcement of UNIX98
	 *     \(lq/bin/sh\(rq emulation for operating system command
	 *     executions
	 *
	 * The features controlled thus are discribed in other sections
	 * of this manual.
	 */
	case 'D':
		BEGIN_EXEC(&States::start);
		expressions.eval();
		if (!expressions.args()) {
			expressions.push(Flags::ed);
		} else {
			tecoInt on = expressions.pop_num_calc();
			tecoInt off = expressions.pop_num_calc(1, ~(tecoInt)0);

			undo.push_var(Flags::ed);
			Flags::ed = (Flags::ed & ~off) | on;
		}
		break;

	/*$
	 * [bool]EX -- Exit program
	 * -EX
	 *
	 * Exits \*(ST.
	 * It is one of the few commands that is not executed
	 * immediately both in batch and interactive mode.
	 * In batch mode EX will exit the program if control
	 * reaches the end of the munged file, preventing
	 * interactive mode editing.
	 * In interactive mode, EX will request an exit that
	 * is performed on command line termination
	 * (i.e. after \fB$$\fP).
	 *
	 * If any buffer is dirty (modified), EX will yield
	 * an error.
	 * When specifying <bool> as a Failure condition
	 * boolean, EX will exit unconditionally.
	 * If <bool> is omitted, the sign prefix is implied
	 * (1 or -1).
	 * In other words \(lq-EX\(rq will always succeed.
	 */
	/** @bug what if changing file after EX? will currently still exit */
	case 'X':
		BEGIN_EXEC(&States::start);

		if (IS_FAILURE(expressions.pop_num_calc()) &&
		    ring.is_any_dirty())
			throw Error("Modified buffers exist");

		undo.push_var<bool>(quit_requested);
		quit_requested = true;
		break;

	default:
		throw SyntaxError(chr);
	}

	return &States::start;
}

static struct ScintillaMessage {
	unsigned int	iMessage;
	uptr_t		wParam;
	sptr_t		lParam;
} scintilla_message = {0, 0, 0};

/*$
 * -- Send Scintilla message
 * [lParam[,wParam]]ESmessage[,wParam]$[lParam]$ -> result
 *
 * Send Scintilla message with code specified by symbolic
 * name <message>, <wParam> and <lParam>.
 * <wParam> may be symbolic when specified as part of the
 * first string argument.
 * If not it is popped from the stack.
 * <lParam> may be specified as a constant string whose
 * pointer is passed to Scintilla if specified as the second
 * string argument.
 * If the second string argument is empty, <lParam> is popped
 * from the stack instead.
 * Parameters popped from the stack may be omitted, in which
 * case 0 is implied.
 * The message's return value is pushed onto the stack.
 *
 * All messages defined by Scintilla (as C macros) can be
 * used by passing their name as a string to ES
 * (e.g. ESSCI_LINESONSCREEN...).
 * The \(lqSCI_\(rq prefix may be omitted and message symbols
 * are case-insensitive.
 * Only the Scintilla lexer symbols (SCLEX_..., SCE_...)
 * may be used symbolically with the ES command as <wParam>,
 * other values must be passed as integers on the stack.
 * In interactive mode, symbols may be auto-completed by
 * pressing Tab.
 * String-building characters are by default interpreted
 * in the string arguments.
 *
 * .BR Warning :
 * Almost all Scintilla messages may be dispatched using
 * this command.
 * \*(ST does not keep track of the editor state changes
 * performed by these commands and cannot undo them.
 * You should never use it to change the editor state
 * (position changes, deletions, etc.) or otherwise
 * rub out will result in an inconsistent editor state.
 * There are however exceptions:
 *   - In the editor profile and batch mode in general,
 *     the ES command may be used freely.
 *   - In the ED hook macro (register \(lq0\(rq),
 *     when a file is added to the ring, most destructive
 *     operations can be performed since rubbing out the
 *     EB command responsible for the hook execution also
 *     removes the buffer from the ring again.
 */
State *
StateScintilla_symbols::done(const gchar *str)
{
	BEGIN_EXEC(&States::scintilla_lparam);

	undo.push_var(scintilla_message);
	if (*str) {
		gchar **symbols = g_strsplit(str, ",", -1);
		tecoInt v;

		if (!symbols[0])
			goto cleanup;
		if (*symbols[0]) {
			v = Symbols::scintilla.lookup(symbols[0], "SCI_");
			if (v < 0)
				throw Error("Unknown Scintilla message symbol \"%s\"",
					    symbols[0]);
			scintilla_message.iMessage = v;
		}

		if (!symbols[1])
			goto cleanup;
		if (*symbols[1]) {
			v = Symbols::scilexer.lookup(symbols[1]);
			if (v < 0)
				throw Error("Unknown Scintilla Lexer symbol \"%s\"",
					    symbols[1]);
			scintilla_message.wParam = v;
		}

		if (!symbols[2])
			goto cleanup;
		if (*symbols[2]) {
			v = Symbols::scilexer.lookup(symbols[2]);
			if (v < 0)
				throw Error("Unknown Scintilla Lexer symbol \"%s\"",
					    symbols[2]);
			scintilla_message.lParam = v;
		}

cleanup:
		g_strfreev(symbols);
	}

	expressions.eval();
	if (!scintilla_message.iMessage) {
		if (!expressions.args())
			throw Error("<ES> command requires at least a message code");

		scintilla_message.iMessage = expressions.pop_num_calc(1, 0);
	}
	if (!scintilla_message.wParam)
		scintilla_message.wParam = expressions.pop_num_calc(1, 0);

	return &States::scintilla_lparam;
}

State *
StateScintilla_lParam::done(const gchar *str)
{
	BEGIN_EXEC(&States::start);

	if (!scintilla_message.lParam)
		scintilla_message.lParam = *str ? (sptr_t)str
						: expressions.pop_num_calc(1, 0);

	expressions.push(interface.ssm(scintilla_message.iMessage,
				       scintilla_message.wParam,
				       scintilla_message.lParam));

	undo.push_var(scintilla_message);
	memset(&scintilla_message, 0, sizeof(scintilla_message));

	return &States::start;
}

/*
 * NOTE: cannot support VideoTECO's <n>I because
 * beginning and end of strings must be determined
 * syntactically
 */
/*$
 * [c1,c2,...]I[text]$ -- Insert text
 *
 * First inserts characters for all the values
 * on the argument stack (interpreted as codepoints).
 * It does so in the order of the arguments, i.e.
 * <c1> is inserted before <c2>, ecetera.
 * Secondly, the command inserts <text>.
 * In interactive mode, <text> is inserted interactively.
 *
 * String building characters are by default enabled for the
 * I command.
 */
void
StateInsert::initial(void)
{
	int args;

	expressions.eval();
	args = expressions.args();
	if (!args)
		return;

	interface.ssm(SCI_BEGINUNDOACTION);
	for (int i = args; i > 0; i--) {
		gchar chr = (gchar)expressions.peek_num(i);
		interface.ssm(SCI_ADDTEXT, 1, (sptr_t)&chr);
	}
	for (int i = args; i > 0; i--)
		expressions.pop_num_calc();
	interface.ssm(SCI_SCROLLCARET);
	interface.ssm(SCI_ENDUNDOACTION);
	ring.dirtify();

	undo.push_msg(SCI_UNDO);
}

void
StateInsert::process(const gchar *str, gint new_chars)
{
	interface.ssm(SCI_BEGINUNDOACTION);
	interface.ssm(SCI_ADDTEXT, new_chars,
		      (sptr_t)(str + strlen(str) - new_chars));
	interface.ssm(SCI_SCROLLCARET);
	interface.ssm(SCI_ENDUNDOACTION);
	ring.dirtify();

	undo.push_msg(SCI_UNDO);
}

State *
StateInsert::done(const gchar *str)
{
	/* nothing to be done when done */
	return &States::start;
}
