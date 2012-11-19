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
#include "qbuffers.h"
#include "parser.h"

//#define DEBUG

gint macro_pc = 0;

namespace States {
	StateStart 		start;
	StateControl		control;
	StateFlowCommand	flowcommand;
	StateCondCommand	condcommand;
	StateECommand		ecommand;
	StateScintilla		scintilla;
	StateInsert		insert;
	StateSearch		search;

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
macro_execute(const gchar *macro) throw (State::Error)
{
	while (macro[macro_pc]) {
#ifdef DEBUG
		g_printf("EXEC(%d): input='%c'/%x, state=%p, mode=%d\n",
			 macro_pc, macro[macro_pc], macro[macro_pc],
			 States::current, mode);
#endif

		State::input(macro[macro_pc]);
		macro_pc++;
	}
}

bool
file_execute(const gchar *filename)
{
	gchar *macro, *p = NULL;

	macro_pc = 0;
	States::current = &States::start;

	if (!g_file_get_contents(filename, &macro, NULL, NULL))
		return false;
	/* only when executing files, ignore Hash-Bang line */
	if (macro[0] == '#')
		p = MAX(strchr(macro, '\r'), strchr(macro, '\n'));

	try {
		macro_execute(p ? p+1 : macro);
	} catch (...) {
		g_free(macro);
		return false;
	}
	g_free(macro);

	macro_pc = 0;
	States::current = &States::start;
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

	/*
	 * FIXME: Q-Register specifications might get more complicated
	 */
	case Machine::STATE_CTL_EU:
	case Machine::STATE_CTL_EQ: {
		QRegister *reg;
		Machine::State state = machine.state;
		machine.state = Machine::STATE_START;

		reg = qregisters[g_ascii_toupper(chr)];
		if (!reg)
			throw InvalidQRegError(chr);

		return state == Machine::STATE_CTL_EQ
			? reg->get_string()
			: g_strdup((gchar []){(gchar)reg->integer, '\0'});
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
		undo.push_var(Modifiers::at);
		Modifiers::at = false;
		undo.push_var(escape_char);
		escape_char = g_ascii_toupper(chr);

		return this;
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
		undo.push_str(strings[0]);
		strings[0] = NULL;
		undo.push_var(escape_char);
		escape_char = '\x1B';
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

StateExpectQReg::StateExpectQReg() : State()
{
	transitions['\0'] = this;
}

State *
StateExpectQReg::custom(gchar chr) throw (Error)
{
	QRegister *reg = qregisters[g_ascii_toupper(chr)];

	if (!reg)
		throw InvalidQRegError(chr);

	return got_register(reg);
}

StateStart::StateStart() : State()
{
	transitions['\0'] = this;
	init(" \f\r\n\v");

	transitions['!'] = &States::label;
	transitions['O'] = &States::gotocmd;
	transitions['^'] = &States::control;
	transitions['F'] = &States::flowcommand;
	transitions['"'] = &States::condcommand;
	transitions['E'] = &States::ecommand;
	transitions['I'] = &States::insert;
	transitions['S'] = &States::search;

	transitions['['] = &States::pushqreg;
	transitions[']'] = &States::popqreg;
	transitions['Q'] = &States::getqreginteger;
	transitions['U'] = &States::setqreginteger;
	transitions['%'] = &States::increaseqreg;
	transitions['M'] = &States::macro;
	transitions['X'] = &States::copytoqreg;
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

		rc = expressions.pop_num_calc(1, qregisters["_"]->integer);
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

	default:
		throw SyntaxError(chr);
	}

	return this;
}

StateFlowCommand::StateFlowCommand() : State()
{
	transitions['\0'] = this;
}

State *
StateFlowCommand::custom(gchar chr) throw (Error)
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
		/* FIXME */
		result = g_ascii_isalnum((gchar)value);
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
	transitions['S'] = &States::scintilla;
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

State *
StateScintilla::done(const gchar *str) throw (Error)
{
	unsigned int	iMessage;
	uptr_t		wParam;
	sptr_t		lParam;

	BEGIN_EXEC(&States::start);

	expressions.eval();
	if (!expressions.args())
		throw Error("<ES> command requires at least a message code");

	iMessage = expressions.pop_num_calc(1, 0);
	wParam = expressions.pop_num_calc(1, 0);
	lParam = *str ? (sptr_t)str : expressions.pop_num_calc(1, 0);

	expressions.push(interface.ssm(iMessage, wParam, lParam));

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

void
StateSearch::initial(void) throw (Error)
{
	gint64 v;

	undo.push_var<Parameters>(parameters);

	parameters.dot = interface.ssm(SCI_GETCURRENTPOS);
	v = expressions.pop_num_calc();
	if (expressions.args()) {
		/* TODO: optional count argument? */
		parameters.count = 1;
		parameters.from = (gint)v;
		parameters.to = (gint)expressions.pop_num_calc();

		if (!Validate::pos(parameters.from) ||
		    !Validate::pos(parameters.to))
			throw RangeError("S");
	} else {
		parameters.count = (gint)v;
		if (v >= 0) {
			parameters.from = parameters.dot;
			parameters.to = interface.ssm(SCI_GETLENGTH);
		} else {
			parameters.from = 0;
			parameters.to = parameters.dot;
		}
	}
}

static inline const gchar *
regexp_escape_chr(gchar chr)
{
	static gchar escaped[] = {'\\', '\0', '\0'};

	escaped[1] = chr;
	return g_ascii_isalnum(chr) ? escaped + 1 : escaped;
}

gchar *
StateSearch::class2regexp(MatchState &state, const gchar *&pattern,
			  bool escape_default)
{
	while (*pattern) {
		QRegister *reg;
		gchar *temp, *temp2;

		switch (state) {
		case STATE_START:
			switch (*pattern) {
			case CTL_KEY('S'):
				return g_strdup("[:^alnum:]");
			case CTL_KEY('E'):
				state = STATE_CTL_E;
				break;
			default:
				temp = escape_default
					? g_strdup(regexp_escape_chr(*pattern))
					: NULL;
				return temp;
			}
			break;

		case STATE_CTL_E:
			switch (g_ascii_toupper(*pattern)) {
			case 'A':
				state = STATE_START;
				return g_strdup("[:alpha:]");
			/* same as <CTRL/S> */
			case 'B':
				state = STATE_START;
				return g_strdup("[:^alnum:]");
			case 'C':
				state = STATE_START;
				return g_strdup("[:alnum:].$");
			case 'D':
				state = STATE_START;
				return g_strdup("[:digit:]");
			case 'G':
				state = STATE_ANYQ;
				break;
			case 'L':
				state = STATE_START;
				return g_strdup("\r\n\v\f");
			case 'R':
				state = STATE_START;
				return g_strdup("[:alnum:]");
			case 'V':
				state = STATE_START;
				return g_strdup("[:lower:]");
			case 'W':
				state = STATE_START;
				return g_strdup("[:upper:]");
			default:
				return NULL;
			}
			break;

		case STATE_ANYQ:
			/* FIXME: Q-Register spec might get more complicated */
			reg = qregisters[g_ascii_toupper(*pattern)];
			if (!reg)
				return NULL;

			temp = reg->get_string();
			temp2 = g_regex_escape_string(temp, -1);
			g_free(temp);
			state = STATE_START;
			return temp2;

		default:
			return NULL;
		}

		pattern++;
	}

	return NULL;
}

gchar *
StateSearch::pattern2regexp(const gchar *&pattern,
			    bool single_expr)
{
	MatchState state = STATE_START;
	gchar *re = NULL;

	while (*pattern) {
		gchar *new_re, *temp;

		temp = class2regexp(state, pattern);
		if (temp) {
			new_re = g_strconcat(re ? : "", "[", temp, "]", NULL);
			g_free(temp);
			g_free(re);
			re = new_re;

			goto next;
		}
		if (!*pattern)
			break;

		switch (state) {
		case STATE_START:
			switch (*pattern) {
			case CTL_KEY('X'): String::append(re, "."); break;
			case CTL_KEY('N'): state = STATE_NOT; break;
			default:
				String::append(re, regexp_escape_chr(*pattern));
			}
			break;

		case STATE_NOT:
			state = STATE_START;
			temp = class2regexp(state, pattern, true);
			if (!temp)
				goto error;
			new_re = g_strconcat(re ? : "", "[^", temp, "]", NULL);
			g_free(temp);
			g_free(re);
			re = new_re;
			g_assert(state == STATE_START);
			break;

		case STATE_CTL_E:
			state = STATE_START;
			switch (g_ascii_toupper(*pattern)) {
			case 'M': state = STATE_MANY; break;
			case 'S': String::append(re, "\\s+"); break;
			/* same as <CTRL/X> */
			case 'X': String::append(re, "."); break;
			/* TODO: ASCII octal code!? */
			case '[':
				String::append(re, "(");
				state = STATE_ALT;
				break;
			default:
				goto error;
			}
			break;

		case STATE_MANY:
			temp = pattern2regexp(pattern, true);
			if (!temp)
				goto error;
			new_re = g_strconcat(re ? : "", "(", temp, ")+", NULL);
			g_free(temp);
			g_free(re);
			re = new_re;
			state = STATE_START;
			break;

		case STATE_ALT:
			switch (*pattern) {
			case ',':
				String::append(re, "|");
				break;
			case ']':
				String::append(re, ")");
				state = STATE_START;
				break;
			default:
				temp = pattern2regexp(pattern, true);
				if (!temp)
					goto error;
				String::append(re, temp);
				g_free(temp);
			}
			break;

		default:
			/* shouldn't happen */
			g_assert(true);
		}

next:
		if (single_expr && state == STATE_START)
			return re;

		pattern++;
	}

	if (state == STATE_ALT)
		String::append(re, ")");

	return re;

error:
	g_free(re);
	return NULL;
}

void
StateSearch::process(const gchar *str,
		     gint new_chars __attribute__((unused))) throw (Error)
{
	static const gint flags = G_REGEX_CASELESS | G_REGEX_MULTILINE |
				  G_REGEX_DOTALL | G_REGEX_RAW;

	QRegister *search_reg = qregisters["_"];
	gchar *re_pattern;
	GRegex *re;
	GMatchInfo *info;
	const gchar *buffer;

	gint matched_from = -1, matched_to = -1;

	undo.push_msg(SCI_GOTOPOS, interface.ssm(SCI_GETCURRENTPOS));

	undo.push_var<gint64>(search_reg->integer);
	search_reg->integer = FAILURE;

	/* NOTE: pattern2regexp() modifies str pointer */
	re_pattern = pattern2regexp(str);
#ifdef DEBUG
	g_printf("REGEXP: %s\n", re_pattern);
#endif
	if (!re_pattern) {
		interface.ssm(SCI_GOTOPOS, parameters.dot);
		return;
	}
	re = g_regex_new(re_pattern, (GRegexCompileFlags)flags,
			 (GRegexMatchFlags)0, NULL);
	g_free(re_pattern);
	if (!re) {
		interface.ssm(SCI_GOTOPOS, parameters.dot);
		return;
	}

	buffer = (const gchar *)interface.ssm(SCI_GETCHARACTERPOINTER);
	g_regex_match_full(re, buffer, (gssize)parameters.to, parameters.from,
			   (GRegexMatchFlags)0, &info, NULL);

	if (parameters.count >= 0) {
		gint count = parameters.count;

		while (g_match_info_matches(info) && --count)
			g_match_info_next(info, NULL);

		if (!count)
			/* successful */
			g_match_info_fetch_pos(info, 0,
					       &matched_from, &matched_to);
	} else {
		/* only keep the last `count' matches, in a circular stack */
		struct Range {
			gint from, to;
		};
		gint count = -parameters.count;
		Range *matched = new Range[count];
		gint matched_total = 0, i = 0;

		while (g_match_info_matches(info)) {
			g_match_info_fetch_pos(info, 0,
					       &matched[i].from,
					       &matched[i].to);

			g_match_info_next(info, NULL);
			i = ++matched_total % count;
		}

		if (matched_total >= count) {
			/* successful, i points to stack bottom */
			matched_from = matched[i].from;
			matched_to = matched[i].to;
		}

		delete matched;
	}

	g_match_info_free(info);

	if (matched_from >= 0 && matched_to >= 0) {
		/* match success */
		search_reg->integer = SUCCESS;
		interface.ssm(SCI_SETSEL, matched_from, matched_to);
	} else {
		interface.ssm(SCI_GOTOPOS, parameters.dot);
	}

	g_regex_unref(re);
}

State *
StateSearch::done(const gchar *str) throw (Error)
{
	BEGIN_EXEC(&States::start);

	QRegister *search_reg = qregisters["_"];

	if (*str) {
		search_reg->undo_set_string();
		search_reg->set_string(str);
	} else {
		gchar *search_str = search_reg->get_string();
		process(search_str, 0 /* unused */);
		g_free(search_str);
	}

	if (eval_colon())
		expressions.push(search_reg->integer);
	else if (IS_FAILURE(search_reg->integer) &&
		 !expressions.find_op(Expressions::OP_LOOP) /* not in loop */)
		interface.msg(Interface::MSG_ERROR, "Search string not found!");

	return &States::start;
}
