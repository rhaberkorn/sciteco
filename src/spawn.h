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

#ifndef __SPAWN_H
#define __SPAWN_H

#include <glib.h>

#include "sciteco.h"
#include "parser.h"
#include "qregisters.h"

namespace SciTECO {

gchar **parse_shell_command_line(const gchar *cmdline, GError **error);

class StateExecuteCommand : public StateExpectString {
public:
	StateExecuteCommand();
	~StateExecuteCommand();

	struct Context {
		GMainContext *mainctx;
		GMainLoop *mainloop;
		GSource *child_src;
		GSource *stdin_src, *stdout_src;

		tecoInt from, to;
		tecoInt start;
		bool text_added;
		GError *error;
	};

private:
	Context ctx;

	void initial(void);
	State *done(const gchar *str);
};

class StateEGCommand : public StateExpectQReg {
public:
	StateEGCommand() : StateExpectQReg(true) {}

private:
	State *got_register(QRegister &reg);
};

namespace States {
	extern StateExecuteCommand	executecommand;
	extern StateEGCommand		egcommand;
}

} /* namespace SciTECO */

#endif
