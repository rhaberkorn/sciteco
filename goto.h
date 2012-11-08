#ifndef __GOTO_H
#define __GOTO_H

#include <glib.h>

#include "sciteco.h"
#include "parser.h"

class StateLabel : public State {
public:
	StateLabel();

private:
	State *custom(gchar chr);
};

void goto_table_clear(void);

#endif
