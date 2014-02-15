/*
 * Copyright (C) 2012-2014 Robin Haberkorn
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

#ifndef __CMDLINE_H
#define __CMDLINE_H

#include <glib.h>

#include "sciteco.h"
#include "parser.h"
#include "qregisters.h"

extern gchar *cmdline;
extern gint cmdline_pos;
extern bool quit_requested;

void cmdline_keypress(gchar key);
static inline void
cmdline_keypress(const gchar *keys)
{
	while (*keys)
		cmdline_keypress(*keys++);
}

void cmdline_fnmacro(const gchar *name);

const gchar *get_eol(void);

/*
 * Command states
 */

class StateSaveCmdline : public StateExpectQReg {
public:
	StateSaveCmdline() : StateExpectQReg(true) {}

private:
	State *got_register(QRegister &reg);
};

namespace States {
	extern StateSaveCmdline save_cmdline;
}

#endif
