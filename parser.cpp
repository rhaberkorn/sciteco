#include <string.h>

#include <glib.h>
#include <glib/gprintf.h>

#include "sciteco.h"
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

bool
macro_execute(const gchar *macro)
{
	while (macro[macro_pc]) {
#ifdef DEBUG
		g_printf("EXEC(%d): input='%c'/%x, state=%p\n",
			 macro_pc, macro[macro_pc], macro[macro_pc],
			 States::current);
#endif

		if (!State::input(macro[macro_pc])) {
			message_display(GTK_MESSAGE_ERROR,
					"Syntax error \"%c\"",
					macro[macro_pc]);
			return false;
		}

		macro_pc++;
	}

	return true;
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

bool
State::input(gchar chr)
{
	State *state = States::current;

	for (;;) {
		State *next = state->get_next_state(chr);

		if (!next)
			/* Syntax error */
			return false;

		if (next == state)
			break;

		state = next;
		chr = '\0';
	}

	if (state != States::current) {
		undo.push_var<State *>(States::current);
		States::current = state;
	}

	return true;
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

	return next;
}

State *
StateExpectString::custom(gchar chr)
{
	gchar insert[255];
	gchar *new_str;

	if (chr == '\0') {
		BEGIN_EXEC(this);
		initial();
		return this;
	}

	/*
	 * String termination handling
	 */
	if (Modifiers::at) {
		undo.push_var<bool>(Modifiers::at);
		Modifiers::at = false;
		undo.push_var<gchar>(escape_char);
		escape_char = g_ascii_toupper(chr);

		return this;
	}

	if (g_ascii_toupper(chr) == escape_char) {
		State *next = done(strings[0] ? : "");

		undo.push_var<gchar>(escape_char);
		escape_char = '\x1B';
		undo.push_str(strings[0]);
		g_free(strings[0]);
		strings[0] = NULL;
		/* TODO: save and reset string building machine state */

		return next;
	}

	/*
	 * String building characters
	 */
	/* TODO */
	insert[0] = chr;
	insert[1] = '\0';

	/*
	 * String accumulation
	 */
	undo.push_str(strings[0]);
	new_str = g_strconcat(strings[0] ? : "", insert, NULL);
	g_free(strings[0]);
	strings[0] = new_str;

	BEGIN_EXEC(this);
	process(strings[0], strlen(insert));
	return this;
}

StateExpectQReg::StateExpectQReg() : State()
{
	transitions['\0'] = this;
}

State *
StateExpectQReg::custom(gchar chr)
{
	QRegister *reg = qregisters[(gchar []){g_ascii_toupper(chr), '\0'}];

	if (!reg)
		return NULL;

	return got_register(reg);
}

StateStart::StateStart() : State()
{
	transitions['\0'] = this;
	init(" \f\r\n\v");

	transitions['!'] = &States::label;
	transitions['^'] = &States::control;
	transitions['F'] = &States::flowcommand;
	transitions['"'] = &States::condcommand;
	transitions['E'] = &States::ecommand;
	transitions['I'] = &States::insert;
	transitions['Q'] = &States::getqreginteger;
	transitions['U'] = &States::setqreginteger;
	transitions['%'] = &States::increaseqreg;
	transitions['M'] = &States::macro;
}

void
StateStart::move(gint64 n)
{
	sptr_t pos = editor_msg(SCI_GETCURRENTPOS);
	editor_msg(SCI_GOTOPOS, pos + n);
	undo.push_msg(SCI_GOTOPOS, pos);
}

void
StateStart::move_lines(gint64 n)
{
	sptr_t pos = editor_msg(SCI_GETCURRENTPOS);
	sptr_t line = editor_msg(SCI_LINEFROMPOSITION, pos);
	editor_msg(SCI_GOTOPOS, editor_msg(SCI_POSITIONFROMLINE, line + n));
	undo.push_msg(SCI_GOTOPOS, pos);
}

State *
StateStart::custom(gchar chr)
{
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
		expressions.push(editor_msg(SCI_GETCURRENTPOS));
		break;

	case 'Z':
		BEGIN_EXEC(this);
		expressions.eval();
		expressions.push(editor_msg(SCI_GETLENGTH));
		break;

	case 'H':
		BEGIN_EXEC(this);
		expressions.eval();
		expressions.push(0);
		expressions.push(editor_msg(SCI_GETLENGTH));
		break;

	/*
	 * control structures (loops)
	 */
	case '<':
		if (mode == MODE_PARSE_ONLY) {
			undo.push_var<gint>(nest_level);
			nest_level++;
			return this;
		}

		expressions.eval();
		if (!expressions.args())
			/* infinite loop */
			expressions.push(-1);

		if (!expressions.peek_num()) {
			expressions.pop_num();

			/* skip to end of loop */
			undo.push_var<Mode>(mode);
			mode = MODE_PARSE_ONLY;
		} else {
			expressions.push(macro_pc);
			expressions.push(Expressions::OP_LOOP);
		}
		break;

	case '>':
		if (mode == MODE_PARSE_ONLY) {
			if (!nest_level) {
				undo.push_var<Mode>(mode);
				mode = MODE_NORMAL;
			} else {
				undo.push_var<gint>(nest_level);
				nest_level--;
			}
		} else {
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

	case ';': {
		BEGIN_EXEC(this);
		/* TODO: search Q-reg */
		gint64 v = expressions.pop_num_calc();
		if (eval_colon())
			v = ~v;

		if (v >= 0) {
			expressions.discard_args();
			g_assert(expressions.pop_op() == Expressions::OP_LOOP);
			expressions.pop_num(); /* pc */
			expressions.pop_num(); /* counter */

			/* skip to end of loop */
			undo.push_var<Mode>(mode);
			mode = MODE_PARSE_ONLY;
		}
		break;
	}

	/*
	 * control structures (conditionals)
	 */
	case '|':
		if (mode == MODE_PARSE_ONLY) {
			if (!skip_else && !nest_level) {
				undo.push_var<Mode>(mode);
				mode = MODE_NORMAL;
			}
			return this;
		}

		/* skip to end of conditional; skip ELSE-part */
		undo.push_var<Mode>(mode);
		mode = MODE_PARSE_ONLY;
		break;

	case '\'':
		if (mode == MODE_NORMAL)
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
		BEGIN_EXEC(this);
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
		undo.push_msg(SCI_GOTOPOS, editor_msg(SCI_GETCURRENTPOS));
		editor_msg(SCI_GOTOPOS, expressions.pop_num_calc(1, 0));
		break;

	case 'C':
		BEGIN_EXEC(this);
		move(expressions.pop_num_calc());
		break;

	case 'R':
		BEGIN_EXEC(this);
		move(-expressions.pop_num_calc());
		break;

	case 'L':
		BEGIN_EXEC(this);
		move_lines(expressions.pop_num_calc());
		break;

	case 'B':
		BEGIN_EXEC(this);
		move_lines(-expressions.pop_num_calc());
		break;

	case '=':
		BEGIN_EXEC(this);
		message_display(GTK_MESSAGE_OTHER, "%" G_GINT64_FORMAT,
				expressions.pop_num_calc());
		break;

	case 'K':
	case 'D': {
		gint64 from, len;

		BEGIN_EXEC(this);
		expressions.eval();

		if (expressions.args() <= 1) {
			from = editor_msg(SCI_GETCURRENTPOS);
			if (chr == 'D') {
				len = expressions.pop_num_calc();
			} else /* chr == 'K' */ {
				sptr_t line = editor_msg(SCI_LINEFROMPOSITION, from) +
					      expressions.pop_num_calc();
				len = editor_msg(SCI_POSITIONFROMLINE, line) - from;
			}
			if (len < 0) {
				from += len;
				len *= -1;
			}
		} else {
			gint64 to = expressions.pop_num();
			from = expressions.pop_num();
			len = to - from;
		}

		if (len > 0) {
			undo.push_msg(SCI_GOTOPOS,
				      editor_msg(SCI_GETCURRENTPOS));
			undo.push_msg(SCI_UNDO);

			editor_msg(SCI_BEGINUNDOACTION);
			editor_msg(SCI_DELETERANGE, from, len);
			editor_msg(SCI_ENDUNDOACTION);
		}
		break;
	}

	default:
		return NULL;
	}

	return this;
}

StateFlowCommand::StateFlowCommand() : State()
{
	transitions['\0'] = this;
}

State *
StateFlowCommand::custom(gchar chr)
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
			mode = MODE_PARSE_ONLY;
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
		mode = MODE_PARSE_ONLY;
		undo.push_var<bool>(skip_else);
		skip_else = true;
		break;

	case '|':
		BEGIN_EXEC(&States::start);
		/* skip to ELSE-part or end of conditional */
		undo.push_var<Mode>(mode);
		mode = MODE_PARSE_ONLY;
		break;

	default:
		return NULL;
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
	gint64 value;
	bool result;

	if (mode == MODE_PARSE_ONLY) {
		undo.push_var<gint>(nest_level);
		nest_level++;
	} else {
		value = expressions.pop_num_calc();
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
		return NULL;
	}

	if (!result) {
		/* skip to ELSE-part or end of conditional */
		undo.push_var<Mode>(mode);
		mode = MODE_PARSE_ONLY;
	}

	return &States::start;
}

StateControl::StateControl() : State()
{
	transitions['\0'] = this;
	transitions['U'] = &States::ctlucommand;
}

State *
StateControl::custom(gchar chr)
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
		return NULL;
	}

	return &States::start;
}

StateECommand::StateECommand() : State()
{
	transitions['\0'] = this;
	transitions['B'] = &States::file;
	transitions['Q'] = &States::eqcommand;
}

State *
StateECommand::custom(gchar chr)
{
	switch (g_ascii_toupper(chr)) {
	case 'X':
		BEGIN_EXEC(&States::start);
		undo.push_var<bool>(quit_requested);
		quit_requested = true;
		break;

	default:
		return NULL;
	}

	return &States::start;
}

/*
 * NOTE: cannot support VideoTECO's <n>I because
 * beginning and end of strings must be determined
 * syntactically
 */
void
StateInsert::initial(void)
{
	int args;

	expressions.eval();
	args = expressions.args();
	if (!args)
		return;

	editor_msg(SCI_BEGINUNDOACTION);
	for (int i = args; i > 0; i--) {
		/* FIXME: if possible prevent pops with index != 1 */
		gchar chr = (gchar)expressions.pop_num_calc(i);
		editor_msg(SCI_ADDTEXT, 1, (sptr_t)&chr);
	}
	editor_msg(SCI_ENDUNDOACTION);

	undo.push_msg(SCI_UNDO);
}

void
StateInsert::process(const gchar *str, gint new_chars)
{
	editor_msg(SCI_BEGINUNDOACTION);
	editor_msg(SCI_ADDTEXT, new_chars,
		   (sptr_t)str + strlen(str) - new_chars);
	editor_msg(SCI_ENDUNDOACTION);

	undo.push_msg(SCI_UNDO);
}

State *
StateInsert::done(const gchar *str __attribute__((unused)))
{
	/* nothing to be done when done */
	return &States::start;
}
