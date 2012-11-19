#ifndef __GOTO_H
#define __GOTO_H

#include <glib.h>
#include <glib/gprintf.h>

#include "sciteco.h"
#include "parser.h"
#include "undo.h"
#include "rbtree.h"

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
	};

	class Label : public RBEntry {
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

public:
	GotoTable() : RBTree() {}

	gint remove(gchar *name);
	gint find(gchar *name);

	gint set(gchar *name, gint pc);
	inline void
	undo_set(gchar *name, gint pc = -1)
	{
		undo.push(new UndoTokenSet(this, name, pc));
	}

#ifdef DEBUG
	void dump(void);
#endif
};

extern GotoTable *goto_table;

/*
 * Command states
 */

class StateLabel : public State {
public:
	StateLabel();

private:
	State *custom(gchar chr) throw (Error);
};

class StateGotoCmd : public StateExpectString {
private:
	State *done(const gchar *str) throw (Error);
};

namespace States {
	extern StateLabel 	label;
	extern StateGotoCmd	gotocmd;
}

#endif
