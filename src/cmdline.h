/*
 * Copyright (C) 2012-2013 Robin Haberkorn
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

extern gchar *cmdline;
extern bool quit_requested;

void cmdline_keypress(gchar key);

/*
 * Command states
 */

class StateSaveCmdline : public StateExpectQReg {
private:
	State *got_register(QRegister *reg) throw (Error);
};

namespace States {
	extern StateSaveCmdline save_cmdline;
}

#endif
