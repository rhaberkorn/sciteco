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

#include <stdarg.h>
#include <string.h>

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
#include "cmdline.h"

//#define DEBUG

gint macro_pc = 0;

namespace States {
	StateStart 		start;
	StateControl		control;
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

void
Execute::step(const gchar *macro) throw (State::Error)
{
	while (macro[macro_pc]) {
#ifdef DEBUG
		g_printf("EXEC(%d): input='%c'/%x, state=%p, mode=%d\n",
			 macro_pc, macro[macro_pc], macro[macro_pc],
			 States::current, mode);
#endif

		if (interface.is_interrupted())
			throw State::Error("Interrupted");

		State::input(macro[macro_pc]);
		macro_pc++;
	}
}

void
Execute::macro(const gchar *macro, bool locals) throw (State::Error)
{
	GotoTable *parent_goto_table = Goto::table;
	GotoTable macro_goto_table(false);

	QRegisterTable *parent_locals = QRegisters::locals;
	QRegisterTable macro_locals(false);

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
	if (locals) {
		macro_locals.initialize();
		QRegisters::locals = &macro_locals;
	}

	try {
		step(macro);
		if (Goto::skip_label)
			throw State::Error("Label \"%s\" not found",
					   Goto::skip_label);
	} catch (...) {
		g_free(Goto::skip_label);
		Goto::skip_label = NULL;

		QRegisters::locals = parent_locals;
		Goto::table = parent_goto_table;

		macro_pc = parent_pc;
		States::current = parent_state;

		throw; /* forward */
	}

	QRegisters::locals = parent_locals;
	Goto::table = parent_goto_table;

	macro_pc = parent_pc;
	States::current = parent_state;
}

bool
Execute::file(const gchar *filename, bool locals)
{
	gchar *macro_str, *p = NULL;

	if (!g_file_get_contents(filename, &macro_str, NULL, NULL))
		return false;
	/* only when executing files, ignore Hash-Bang line */
	if (*macro_str == '#')
		p = MAX(strchr(macro_str, '\r'), strchr(macro_str, '\n'));

	try {
		macro(p ? p+1 : macro_str, locals);
	} catch (...) {
		g_free(macro_str);
		return false;
	}

	g_free(macro_str);
	return true;
}

State::Error::Error(const gchar *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	interface.vmsg(Interface::MSG_ERROR, fmt, ap);
	va_end(ap);
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
State::input(gchar chr) throw (Error)
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
State::get_next_state(gchar chr) throw (Error)
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

gchar *
StateExpectString::machine_input(gchar chr) throw (Error)
{
	switch (machine.mode) {
	case Machine::MODE_UPPER:
		chr = g_ascii_toupper(chr);
		break;
	case Machine::MODE_LOWER:
		chr = g_ascii_tolower(chr);
		break;
	default:
		break;
	}

	if (machine.toctl) {
		chr = CTL_KEY(g_ascii_toupper(chr));
		machine.toctl = false;
	}

	if (machine.state == Machine::STATE_ESCAPED) {
		machine.state = Machine::STATE_START;
		goto original;
	}

	if (chr == '^') {
		machine.toctl = true;
		return NULL;
	}

	switch (machine.state) {
	case Machine::STATE_START:
		switch (chr) {
		case CTL_KEY('Q'):
		case CTL_KEY('R'): machine.state = Machine::STATE_ESCAPED; break;
		case CTL_KEY('V'): machine.state = Machine::STATE_LOWER; break;
		case CTL_KEY('W'): machine.state = Machine::STATE_UPPER; break;
		case CTL_KEY('E'): machine.state = Machine::STATE_CTL_E; break;
		default:
			goto original;
		}
		break;

	case Machine::STATE_LOWER:
		machine.state = Machine::STATE_START;
		if (chr != CTL_KEY('V'))
			return g_strdup((gchar []){g_ascii_tolower(chr), '\0'});
		machine.mode = Machine::MODE_LOWER;
		break;

	case Machine::STATE_UPPER:
		machine.state = Machine::STATE_START;
		if (chr != CTL_KEY('W'))
			return g_strdup((gchar []){g_ascii_toupper(chr), '\0'});
		machine.mode = Machine::MODE_UPPER;
		break;

	case Machine::STATE_CTL_E:
		switch (g_ascii_toupper(chr)) {
		case 'Q': machine.state = Machine::STATE_CTL_EQ; break;
		case 'U': machine.state = Machine::STATE_CTL_EU; break;
		default:
			machine.state = Machine::STATE_START;
			return g_strdup((gchar []){CTL_KEY('E'), chr, '\0'});
		}
		break;

	case Machine::STATE_CTL_EU:
		if (chr == '.') {
			machine.state = Machine::STATE_CTL_EU_LOCAL;
		} else {
			QRegister *reg;

			machine.state = Machine::STATE_START;
			reg = QRegisters::globals[g_ascii_toupper(chr)];
			if (!reg)
				throw InvalidQRegError(chr);
			return g_strdup(CHR2STR(reg->get_integer()));
		}
		break;

	case Machine::STATE_CTL_EU_LOCAL: {
		QRegister *reg;

		machine.state = Machine::STATE_START;
		reg = (*QRegisters::locals)[g_ascii_toupper(chr)];
		if (!reg)
			throw InvalidQRegError(chr, true);
		return g_strdup(CHR2STR(reg->get_integer()));
	}

	case Machine::STATE_CTL_EQ:
		if (chr == '.') {
			machine.state = Machine::STATE_CTL_EQ_LOCAL;
		} else {
			QRegister *reg;

			machine.state = Machine::STATE_START;
			reg = QRegisters::globals[g_ascii_toupper(chr)];
			if (!reg)
				throw InvalidQRegError(chr);
			return reg->get_string();
		}
		break;

	case Machine::STATE_CTL_EQ_LOCAL: {
		QRegister *reg;

		machine.state = Machine::STATE_START;
		reg = (*QRegisters::locals)[g_ascii_toupper(chr)];
		if (!reg)
			throw InvalidQRegError(chr, true);
		return reg->get_string();
	}

	default:
		g_assert(TRUE);
	}

	return NULL;

original:
	return g_strdup((gchar []){chr, '\0'});
}

State *
StateExpectString::custom(gchar chr) throw (Error)
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
			undo.push_var(nesting);
			nesting++;
			break;
		case '}':
			undo.push_var(nesting);
			nesting--;
			break;
		}
	} else if (g_ascii_toupper(chr) == escape_char) {
		undo.push_var(nesting);
		nesting--;
	}

	if (!nesting) {
		State *next;
		gchar *string = strings[0];

		undo.push_str(strings[0]) = NULL;
		if (last)
			undo.push_var(escape_char) = '\x1B';
		nesting = 1;

		if (string_building) {
			undo.push_var(machine);
			machine.state = Machine::STATE_START;
			machine.mode = Machine::MODE_NORMAL;
			machine.toctl = false;
		}

		next = done(string ? : "");
		g_free(string);
		return next;
	}

	BEGIN_EXEC(this);

	/*
	 * String building characters
	 */
	if (string_building) {
		undo.push_var(machine);
		insert = machine_input(chr);
		if (!insert)
			return this;
	} else {
		insert = g_strdup((gchar []){chr, '\0'});
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
StateStart::insert_integer(gint64 v)
{
	gchar buf[64+1]; /* maximum length if radix = 2 */
	gchar *p = buf + sizeof(buf);

	*--p = '\0';
	interface.ssm(SCI_BEGINUNDOACTION);
	if (v < 0) {
		interface.ssm(SCI_ADDTEXT, 1, (sptr_t)"-");
		v *= -1;
	}
	do {
		*--p = '0' + (v % expressions.radix);
		if (*p > '9')
			*p += 'A' - '9';
	} while ((v /= expressions.radix));
	interface.ssm(SCI_ADDTEXT, buf + sizeof(buf) - p - 1,
		      (sptr_t)p);
	interface.ssm(SCI_SCROLLCARET);
	interface.ssm(SCI_ENDUNDOACTION);
	ring.dirtify();

	undo.push_msg(SCI_UNDO);
}

gint64
StateStart::read_integer(void)
{
	uptr_t pos = interface.ssm(SCI_GETCURRENTPOS);
	gchar c = (gchar)interface.ssm(SCI_GETCHARAT, pos);
	gint64 v = 0;
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
StateStart::move_chars(gint64 n)
{
	sptr_t pos = interface.ssm(SCI_GETCURRENTPOS);

	if (!Validate::pos(pos + n))
		return FAILURE;

	interface.ssm(SCI_GOTOPOS, pos + n);
	undo.push_msg(SCI_GOTOPOS, pos);
	return SUCCESS;
}

tecoBool
StateStart::move_lines(gint64 n)
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
StateStart::delete_words(gint64 n)
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
StateStart::custom(gchar chr) throw (Error)
{
	gint64 v;
	tecoBool rc;

	/*
	 * <CTRL/x> commands implemented in StateCtrlCmd
	 */
	if (IS_CTL(chr))
		return States::control.get_next_state(CTL_ECHO(chr));

	/*
	 * arithmetics
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

	case '.':
		BEGIN_EXEC(this);
		expressions.eval();
		expressions.push(interface.ssm(SCI_GETCURRENTPOS));
		break;

	case 'Z':
		BEGIN_EXEC(this);
		expressions.eval();
		expressions.push(interface.ssm(SCI_GETLENGTH));
		break;

	case 'H':
		BEGIN_EXEC(this);
		expressions.eval();
		expressions.push(0);
		expressions.push(interface.ssm(SCI_GETLENGTH));
		break;

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
			gint64 loop_pc, loop_cnt;

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

	case ';':
		BEGIN_EXEC(this);

		v = QRegisters::globals["_"]->get_integer();
		rc = expressions.pop_num_calc(1, v);
		if (eval_colon())
			rc = ~rc;

		if (IS_FAILURE(rc)) {
			expressions.discard_args();
			g_assert(expressions.pop_op() == Expressions::OP_LOOP);
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

	case 'C':
		BEGIN_EXEC(this);
		rc = move_chars(expressions.pop_num_calc());
		if (eval_colon())
			expressions.push(rc);
		else if (IS_FAILURE(rc))
			throw MoveError("C");
		break;

	case 'R':
		BEGIN_EXEC(this);
		rc = move_chars(-expressions.pop_num_calc());
		if (eval_colon())
			expressions.push(rc);
		else if (IS_FAILURE(rc))
			throw MoveError("R");
		break;

	case 'L':
		BEGIN_EXEC(this);
		rc = move_lines(expressions.pop_num_calc());
		if (eval_colon())
			expressions.push(rc);
		else if (IS_FAILURE(rc))
			throw MoveError("L");
		break;

	case 'B':
		BEGIN_EXEC(this);
		rc = move_lines(-expressions.pop_num_calc());
		if (eval_colon())
			expressions.push(rc);
		else if (IS_FAILURE(rc))
			throw MoveError("B");
		break;

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

	case 'V':
		BEGIN_EXEC(this);
		rc = delete_words(expressions.pop_num_calc());
		if (eval_colon())
			expressions.push(rc);
		else if (IS_FAILURE(rc))
			throw Error("Not enough words to delete with <V>");
		break;

	case 'Y':
		BEGIN_EXEC(this);
		rc = delete_words(-expressions.pop_num_calc());
		if (eval_colon())
			expressions.push(rc);
		else if (IS_FAILURE(rc))
			throw Error("Not enough words to delete with <Y>");
		break;

	case '=':
		BEGIN_EXEC(this);
		interface.msg(Interface::MSG_USER, "%" G_GINT64_FORMAT,
			      expressions.pop_num_calc());
		break;

	case 'K':
	case 'D': {
		gint64 from, len;

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
			gint64 to = expressions.pop_num();
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
StateFCommand::custom(gchar chr) throw (Error)
{
	switch (chr) {
	/*
	 * loop flow control
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

	case '>': {
		gint64 loop_pc, loop_cnt;

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
	case '\'':
		BEGIN_EXEC(&States::start);
		/* skip to end of conditional */
		undo.push_var<Mode>(mode);
		mode = MODE_PARSE_ONLY_COND;
		undo.push_var<bool>(skip_else);
		skip_else = true;
		break;

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
StateCondCommand::custom(gchar chr) throw (Error)
{
	gint64 value = 0;
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
	case 'E':
	case 'F':
	case 'U':
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
	case 'S':
	case 'T':
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
}

State *
StateControl::custom(gchar chr) throw (Error)
{
	switch (g_ascii_toupper(chr)) {
	case 'O':
		BEGIN_EXEC(&States::start);
		expressions.set_radix(8);
		break;

	case 'D':
		BEGIN_EXEC(&States::start);
		expressions.set_radix(10);
		break;

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
	case 'I':
		BEGIN_EXEC(&States::insert);
		expressions.eval();
		expressions.push('\t');
		return &States::insert;

	/*
	 * Alternatives: ^[, <CTRL/[>, <ESC>
	 */
	case '[':
		BEGIN_EXEC(&States::start);
		expressions.discard_args();
		break;

	/*
	 * Additional numeric operations
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

StateECommand::StateECommand() : State()
{
	transitions['\0'] = this;
	transitions['B'] = &States::editfile;
	transitions['S'] = &States::scintilla_symbols;
	transitions['Q'] = &States::eqcommand;
	transitions['W'] = &States::savefile;
}

State *
StateECommand::custom(gchar chr) throw (Error)
{
	switch (g_ascii_toupper(chr)) {
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

	case 'D':
		BEGIN_EXEC(&States::start);
		expressions.eval();
		if (!expressions.args()) {
			expressions.push(Flags::ed);
		} else {
			gint64 on = expressions.pop_num_calc();
			gint64 off = expressions.pop_num_calc(1, ~(gint64)0);

			undo.push_var(Flags::ed);
			Flags::ed = (Flags::ed & ~off) | on;
		}
		break;

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

State *
StateScintilla_symbols::done(const gchar *str) throw (Error)
{
	BEGIN_EXEC(&States::scintilla_lparam);

	undo.push_var(scintilla_message);
	if (*str) {
		gchar **symbols = g_strsplit(str, ",", -1);
		gint64 v;

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
StateScintilla_lParam::done(const gchar *str) throw (Error)
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
void
StateInsert::initial(void) throw (Error)
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
StateInsert::process(const gchar *str, gint new_chars) throw (Error)
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
StateInsert::done(const gchar *str __attribute__((unused))) throw (Error)
{
	/* nothing to be done when done */
	return &States::start;
}
