#include <bsd/sys/tree.h>

#include <glib.h>
#include <glib/gprintf.h>

#include "sciteco.h"
#include "expressions.h"
#include "parser.h"
#include "undo.h"
#include "rbtree.h"
#include "goto.h"

namespace States {
	StateLabel	label;
	StateGotoCmd	gotocmd;
}

static gchar *skip_label = NULL;

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
			g_free(name);
			name = NULL;

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

	gint
	remove(gchar *name)
	{
		gint existing_pc = -1;

		Label label(name);
		Label *existing = (Label *)RBTree::find(&label);

		if (existing) {
			existing_pc = existing->pc;
			RBTree::remove(existing);
			delete existing;
		}

		return existing_pc;
	}

	gint
	find(gchar *name)
	{
		Label label(name);
		Label *existing = (Label *)RBTree::find(&label);

		return existing ? existing->pc : -1;
	}

	gint
	set(gchar *name, gint pc)
	{
		if (pc < 0)
			return remove(name);

		Label *label = new Label(name, pc);
		Label *existing;
		gint existing_pc = -1;

		existing = (Label *)RBTree::find(label);
		if (existing) {
			existing_pc = existing->pc;
			g_free(existing->name);
			existing->name = label->name;
			existing->pc = label->pc;

			label->name = NULL;
			delete label;
		} else {
			RBTree::insert(label);
		}

#ifdef DEBUG
		dump();
#endif

		return existing_pc;
	}

	inline void
	undo_set(gchar *name, gint pc = -1)
	{
		undo.push(new UndoTokenSet(this, name, pc));
	}

	void
	clear(void)
	{
		Label *cur;

		while ((cur = (Label *)RBTree::min())) {
			RBTree::remove(cur);
			delete cur;
		}
	}

#ifdef DEBUG
	void
	dump(void)
	{
		for (Label *cur = (Label *)RBTree::min();
		     cur != NULL;
		     cur = (Label *)cur->next())
			g_printf("table[\"%s\"] = %d\n", cur->name, cur->pc);
		g_printf("---END---\n");
	}
#endif
};

static GotoTable table;

void
goto_table_clear(void)
{
	table.clear();
}

/*
 * Command states
 */

StateLabel::StateLabel() : State()
{
	transitions['\0'] = this;
}

State *
StateLabel::custom(gchar chr) throw (Error)
{
	gchar *new_str;

	if (chr == '!') {
		table.undo_set(strings[0], table.set(strings[0], macro_pc));

		if (!g_strcmp0(strings[0], skip_label)) {
			undo.push_str(skip_label);
			g_free(skip_label);
			skip_label = NULL;

			undo.push_var<Mode>(mode);
			mode = MODE_NORMAL;
		}

		undo.push_str(strings[0]);
		g_free(strings[0]);
		strings[0] = NULL;

		return &States::start;
	}

	undo.push_str(strings[0]);
	new_str = g_strdup_printf("%s%c", strings[0] ? : "", chr);
	g_free(strings[0]);
	strings[0] = new_str;

	return this;
}

State *
StateGotoCmd::done(const gchar *str) throw (Error)
{
	gint64 value;
	gchar **labels;

	BEGIN_EXEC(&States::start);

	value = expressions.pop_num_calc();
	labels = g_strsplit(str, ",", -1);

	if (value > 0 && value <= g_strv_length(labels) && *labels[value-1]) {
		gint pc = table.find(labels[value-1]);

		if (pc >= 0) {
			macro_pc = pc;
		} else {
			/* skip till label is defined */
			undo.push_str(skip_label);
			skip_label = g_strdup(labels[value-1]);
			undo.push_var<Mode>(mode);
			mode = MODE_PARSE_ONLY_GOTO;
		}
	}

	g_strfreev(labels);
	return &States::start;
}
