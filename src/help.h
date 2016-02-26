/*
 * Copyright (C) 2012-2016 Robin Haberkorn
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

#ifndef __HELP_H
#define __HELP_H

#include <string.h>

#include <glib.h>
#include <glib/gprintf.h>

#include "sciteco.h"
#include "parser.h"
#include "undo.h"
#include "rbtree.h"

namespace SciTECO {

class HelpIndex : public RBTree {
public:
	class Topic : public RBTree::RBEntry {
	public:
		gchar	*name;
		gchar	*filename;
		tecoInt	pos;

		Topic(const gchar *_name, const gchar *_filename = NULL, tecoInt _pos = 0)
		     : name(g_strdup(_name)),
		       filename(_filename ? g_strdup(_filename) : NULL),
		       pos(_pos) {}
		~Topic()
		{
			g_free(name);
			g_free(filename);
		}

		int
		operator <(RBEntry &l2)
		{
			return g_ascii_strcasecmp(name, ((Topic &)l2).name);
		}
	};

	void load(void);

	Topic *find(const gchar *name);

	void set(const gchar *name, const gchar *filename,
	         tecoInt pos = 0);
};

extern HelpIndex help_index;

/*
 * Command states
 */

class StateGetHelp : public StateExpectString {
public:
	StateGetHelp() : StateExpectString(false) {}

private:
	void initial(void);
	State *done(const gchar *str);
};

namespace States {
	extern StateGetHelp gethelp;
}

} /* namespace SciTECO */

#endif
