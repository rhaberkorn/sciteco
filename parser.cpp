#include <glib.h>
#include <glib/gprintf.h>

#include "sciteco.h"
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

State *
StateStart::custom(gchar chr)
{
#if 0
	if (IS_CTL(chr))
		return states.ctl.get_next_state(CTL_ECHO(chr));
#endif

	switch (g_ascii_toupper(chr)) {
	/*
	 * commands
	 */
 	case 'C':
 		editor_msg(SCI_CHARRIGHT);
 		undo.push_msg(SCI_CHARLEFT);
 		break;
 	default:
 		return NULL;
	}

	return this;
}
