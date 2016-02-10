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

#ifndef __GOTO_H
#define __GOTO_H

#include <string.h>

#include <glib.h>
#include <glib/gprintf.h>

#include "sciteco.h"
#include "parser.h"
#include "undo.h"
#include "rbtree.h"

namespace SciTECO {

class GotoTable : public RBTree {
	class UndoTokenSet : public UndoToken {
		GotoTable *table;

		gchar	*name;
		gint	pc;

	public:
		UndoTokenSet(GotoTable *_table, gchar *_name, gint _pc = -1)
			    : table(_table), name(g_strdup(_name)), pc(_pc) {}
		~UndoTokenSet()
		{
			g_free(name);
		}

		void
		run(void)
		{
			table->set(name, pc);
#ifdef DEBUG
			table->dump();
#endif
		}

		gsize
		get_size(void) const
		{
			return sizeof(*this) + strlen(name) + 1;
		}
	};

	class Label : public RBTree::RBEntry {
	public:
		gchar	*name;
		gint	pc;

		Label(gchar *_name, gint _pc = -1)
		     : name(g_strdup(_name)), pc(_pc) {}
		~Label()
		{
			g_free(name);
		}

		int
		operator <(RBEntry &l2)
		{
			return g_strcmp0(name, ((Label &)l2).name);
		}
	};

	/*
	 * whether to generate UndoTokens (unnecessary in macro invocations)
	 */
	bool must_undo;

public:
	GotoTable(bool _undo = true) : RBTree(), must_undo(_undo) {}

	gint remove(gchar *name);
	gint find(gchar *name);

	gint set(gchar *name, gint pc);
	inline void
	undo_set(gchar *name, gint pc = -1)
	{
		if (must_undo)
			undo.push<UndoTokenSet>(this, name, pc);
	}

#ifdef DEBUG
	void dump(void);
#endif
};

namespace Goto {
	extern GotoTable *table;
	extern gchar *skip_label;
}

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
	extern StateLabel	label;
	extern StateGotoCmd	gotocmd;
}

} /* namespace SciTECO */

#endif
