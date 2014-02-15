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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

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

namespace Goto {
	GotoTable *table = NULL;
	gchar *skip_label = NULL;
}

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
StateLabel::custom(gchar chr)
{
	if (chr == '!') {
		Goto::table->undo_set(strings[0],
				      Goto::table->set(strings[0], macro_pc));

		if (!g_strcmp0(strings[0], Goto::skip_label)) {
			g_free(undo.push_str(Goto::skip_label));
			Goto::skip_label = NULL;

			undo.push_var(mode) = MODE_NORMAL;
		}

		g_free(undo.push_str(strings[0]));
		strings[0] = NULL;

		return &States::start;
	}

	String::append(undo.push_str(strings[0]), chr);
	return this;
}

/*$
 * Olabel$ -- Go to label
 * [n]Olabel1[,label2,...]$
 *
 * Go to <label>.
 * The simple go-to command is a special case of the
 * computed go-to command.
 * A comma-separated list of labels may be specified
 * in the string argument.
 * The label to jump to is selected by <n> (1 is <label1>,
 * 2 is <label2>, etc.).
 * If <n> is omitted, the sign prefix is implied.
 *
 * If the label selected by <n> is does not exist in the
 * list of labels, the command does nothing.
 * Label definitions are cached in a table, so that
 * if the label to go to has already been defined, the
 * go-to command will jump immediately.
 * Otherwise, parsing continues until the <label>
 * is defined.
 * The command will yield an error if a label has
 * not been defined when the macro or command-line
 * is terminated.
 * In the latter case, the user will not be able to
 * terminate the command-line.
 */
State *
StateGotoCmd::done(const gchar *str)
{
	tecoInt value;
	gchar **labels;

	BEGIN_EXEC(&States::start);

	value = expressions.pop_num_calc();
	labels = g_strsplit(str, ",", -1);

	if (value > 0 && value <= (tecoInt)g_strv_length(labels) &&
	    *labels[value-1]) {
		gint pc = Goto::table->find(labels[value-1]);

		if (pc >= 0) {
			macro_pc = pc;
		} else {
			/* skip till label is defined */
			undo.push_str(Goto::skip_label);
			Goto::skip_label = g_strdup(labels[value-1]);
			undo.push_var(mode) = MODE_PARSE_ONLY_GOTO;
		}
	}

	g_strfreev(labels);
	return &States::start;
}
