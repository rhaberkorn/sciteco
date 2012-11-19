#include <glib.h>
#include <glib/gprintf.h>

#include "sciteco.h"
#include "expressions.h"
#include "parser.h"
#include "undo.h"
#include "goto.h"

namespace States {
	StateLabel	label;
	StateGotoCmd	gotocmd;
}

static gchar *skip_label = NULL;

GotoTable *goto_table = NULL;

gint
GotoTable::remove(gchar *name)
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
GotoTable::find(gchar *name)
{
	Label label(name);
	Label *existing = (Label *)RBTree::find(&label);

	return existing ? existing->pc : -1;
}

gint
GotoTable::set(gchar *name, gint pc)
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

#ifdef DEBUG
void
GotoTable::dump(void)
{
	for (Label *cur = (Label *)RBTree::min();
	     cur != NULL;
	     cur = (Label *)cur->next())
		g_printf("table[\"%s\"] = %d\n", cur->name, cur->pc);
	g_printf("---END---\n");
}
#endif

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
		goto_table->undo_set(strings[0],
				     goto_table->set(strings[0], macro_pc));

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
		gint pc = goto_table->find(labels[value-1]);

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
