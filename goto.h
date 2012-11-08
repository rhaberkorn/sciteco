#ifndef __GOTO_H
#define __GOTO_H

#include <glib.h>

#include "sciteco.h"
#include "parser.h"

class StateLabel : public State {
private:
	State *custom(gchar chr);
};

#endif
