#include <glib.h>
#include <glib/gprintf.h>

#include "sciteco.h"
#include "undo.h"
#include "expressions.h"
#include "parser.h"

gint macro_pc = 0;

static struct {
	StateStart	start;
} states;

static State *current_state = &states.start;

static struct {
	bool colon;
	bool at;
} modifiers = {false, false};

static enum {
	MODE_NORMAL = 0,
	MODE_PARSE_ONLY
} mode = MODE_NORMAL;

#define BEGIN_EXEC(STATE) G_STMT_START {	\
	if (mode != MODE_NORMAL)		\
		return STATE;			\
} G_STMT_END

bool
macro_execute(const gchar *macro)
{
	while (macro[macro_pc]) {
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
	if (!modifiers.colon)
		return false;

	undo.push_var<bool>(modifiers.colon);
	modifiers.colon = false;
	return true;
}

bool
State::input(gchar chr)
{
	State *state = current_state;

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

	if (state != current_state) {
		undo.push_var<State *>(current_state);
		current_state = state;
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

StateStart::StateStart() : State()
{
	transitions['\0'] = this;
	init(" \r\n\v");
}

void
StateStart::move(gint64 n)
{
	sptr_t pos = editor_msg(SCI_GETCURRENTPOS);
	editor_msg(SCI_GOTOPOS, pos + n);
	undo.push_msg(SCI_GOTOPOS, pos);
}

State *
StateStart::custom(gchar chr)
{
#if 0
	/*
	 * <CTRL/x> commands implemented in StateCtrlCmd
	 */
	if (IS_CTL(chr))
		return states.ctl.get_next_state(CTL_ECHO(chr));
#endif

	/*
	 * arithmetics
	 */
	if (g_ascii_isdigit(chr)) {
		BEGIN_EXEC(this);
		expressions.add_digit(chr);
		return this;
	}

	switch (g_ascii_toupper(chr)) {
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
	 * commands
	 */
	case 'C':
		BEGIN_EXEC(this);
		move(expressions.pop_num_calc());
		break;
	case 'R':
		BEGIN_EXEC(this);
		move(-expressions.pop_num_calc());
		break;
	case '=':
		BEGIN_EXEC(this);
		message_display(GTK_MESSAGE_OTHER, "%" G_GINT64_FORMAT,
				expressions.pop_num_calc());
		break;
	default:
		return NULL;
	}

	return this;
}

