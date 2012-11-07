#include <glib.h>
#include <glib/gprintf.h>

#include "sciteco.h"
#include "undo.h"
#include "expressions.h"
#include "parser.h"

gint macro_pc = 0;

static struct {
	StateStart start;
} states;

static State *current_state = &states.start;

gboolean
macro_execute(const gchar *macro)
{
	while (macro[macro_pc]) {
		if (!State::input(macro[macro_pc])) {
			message_display(GTK_MESSAGE_ERROR,
					"Syntax error \"%c\"",
					macro[macro_pc]);
			return FALSE;
		}

		macro_pc++;
	}

	return TRUE;
}

State::State()
{
	for (int i = 0; i < MAX_TRANSITIONS; i++)
		transitions[i] = NULL;
}

gboolean
State::input(gchar chr)
{
	State *state = current_state;

	for (;;) {
		State *next = state->get_next_state(chr);

		if (!next)
			/* Syntax error */
			return FALSE;

		if (next == state)
			break;

		state = next;
		chr = '\0';
	}

	if (state != current_state) {
		undo.push_var<State *>(current_state);
		current_state = state;
	}

	return TRUE;
}

StateStart::StateStart() : State()
{
	transitions['\0']	= this;
	transitions[' ']	= this;
	transitions['\r']	= this;
	transitions['\n']	= this;
	transitions['\v']	= this;
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
		expressions.add_digit(chr);
		return this;
	}

	switch (g_ascii_toupper(chr)) {
	case '/':
		expressions.push_calc(Expressions::OP_DIV);
		break;
	case '*':
		expressions.push_calc(Expressions::OP_MUL);
		break;
	case '+':
		expressions.push_calc(Expressions::OP_ADD);
		break;
	case '-':
		if (!expressions.args() ||
		    expressions.peek_num() == G_MAXINT64)
			expressions.set_num_sign(-expressions.num_sign);
		else
			expressions.push_calc(Expressions::OP_SUB);
		break;
	case '&':
		expressions.push_calc(Expressions::OP_AND);
		break;
	case '#':
		expressions.push_calc(Expressions::OP_OR);
		break;
	case '(':
		if (expressions.num_sign < 0) {
			expressions.push(-1);
			expressions.push_calc(Expressions::OP_MUL);
		}
		expressions.push(Expressions::OP_BRACE);
		break;
	case ')':
		expressions.eval(true);
		break;
	case ',':
		expressions.eval();
		expressions.push(G_MAXINT64);
		break;
	/*
	 * commands
	 */
	case 'C':
		move(expressions.pop_num_calc());
		break;
	case 'R':
		move(-expressions.pop_num_calc());
		break;
	case '=':
		message_display(GTK_MESSAGE_OTHER, "%" G_GINT64_FORMAT,
				expressions.pop_num_calc());
		break;
	default:
		return NULL;
	}

	return this;
}
