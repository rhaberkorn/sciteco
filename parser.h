#ifndef __PARSER_H
#define __PARSER_H

#include <glib.h>
#include <glib/gprintf.h>

#include "sciteco.h"

class State {
protected:
	/* static transitions */
	State *transitions[MAX_TRANSITIONS];

public:
	State();

	static gboolean input(gchar chr);
	State *get_next_state(gchar chr);

	virtual State *
	custom(gchar chr)
	{
		return NULL;
	}
};

class StateStart : public State {
public:
	StateStart();

	void move(gint64 n);

	State *custom(gchar chr);
};

#endif
