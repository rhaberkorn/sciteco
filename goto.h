#ifndef __GOTO_H
#define __GOTO_H

#include <glib.h>

#include "sciteco.h"
#include "parser.h"

/*
 * Command states
 */

class StateLabel : public State {
public:
	StateLabel();

private:
	State *custom(gchar chr);
};

class StateGotoCmd : public StateExpectString {
private:
	State *done(const gchar *str);
};

namespace States {
	extern StateLabel 	label;
	extern StateGotoCmd	gotocmd;
}

void goto_table_clear(void);

#endif
