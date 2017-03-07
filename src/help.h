/*
 * Copyright (C) 2012-2017 Robin Haberkorn
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
#include "memory.h"
#include "parser.h"
#include "undo.h"
#include "rbtree.h"

namespace SciTECO {

class HelpIndex : private RBTreeStringCase, public Object {
public:
	class Topic : public RBEntryOwnString {
	public:
		gchar	*filename;
		tecoInt	pos;

		Topic(const gchar *name, const gchar *_filename = NULL, tecoInt _pos = 0)
		     : RBEntryOwnString(name),
		       filename(_filename ? g_strdup(_filename) : NULL),
		       pos(_pos) {}
		~Topic()
		{
			g_free(filename);
		}
	};

	~HelpIndex()
	{
		Topic *cur;

		while ((cur = (Topic *)root()))
			delete (Topic *)remove(cur);
	}

	void load(void);

	Topic *find(const gchar *name);

	void set(const gchar *name, const gchar *filename,
	         tecoInt pos = 0);

	inline gchar *
	auto_complete(const gchar *name, gchar completed = '\0')
	{
		return RBTreeStringCase::auto_complete(name, completed);
	}
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

protected:
	/* in cmdline.cpp */
	void process_edit_cmd(gchar key);
};

namespace States {
	extern StateGetHelp gethelp;
}

} /* namespace SciTECO */

#endif
