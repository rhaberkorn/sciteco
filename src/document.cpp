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

#include <Scintilla.h>

#include "sciteco.h"
#include "interface.h"
#include "undo.h"
#include "document.h"

namespace SciTECO {

static inline void
set_representations(void)
{
	static const char *reps[] = {
		"^@", "^A", "^B", "^C", "^D", "^E", "^F", "^G",
		"^H", "TAB" /* ^I */, "LF" /* ^J */, "^K", "^L", "CR" /* ^M */, "^N", "^O",
		"^P", "^Q", "^R", "^S", "^T", "^U", "^V", "^W",
		"^X", "^Y", "^Z", "$" /* ^[ */, "^\\", "^]", "^^", "^_"
	};

	for (guint cc = 0; cc < G_N_ELEMENTS(reps); cc++) {
		gchar buf[] = {(gchar)cc, '\0'};
		interface.ssm(SCI_SETREPRESENTATION,
			      (uptr_t)buf, (sptr_t)reps[cc]);
	}
}

class UndoSetRepresentations : public UndoToken {
public:
	void
	run(void)
	{
		set_representations();
	}
};

void
Document::edit(void)
{
	if (!is_initialized())
		doc = (SciDoc)interface.ssm(SCI_CREATEDOCUMENT);

	interface.ssm(SCI_SETDOCPOINTER, 0, (sptr_t)doc);
	interface.ssm(SCI_SETFIRSTVISIBLELINE, first_line);
	interface.ssm(SCI_SETXOFFSET, xoffset);
	interface.ssm(SCI_SETSEL, anchor, (sptr_t)dot);

	/*
	 * Default TECO-style character representations.
	 * They are reset on EVERY SETDOCPOINTER call by Scintilla.
	 */
	set_representations();
}

void
Document::undo_edit(void)
{
	if (!is_initialized())
		doc = (SciDoc)interface.ssm(SCI_CREATEDOCUMENT);

	/*
	 * see above: set TECO-style character representations
	 * NOTE: could be done with push_msg() but that requires
	 * making the entire mapping static constant
	 */
	undo.push(new UndoSetRepresentations());

	undo.push_msg(SCI_SETSEL, anchor, (sptr_t)dot);
	undo.push_msg(SCI_SETXOFFSET, xoffset);
	undo.push_msg(SCI_SETFIRSTVISIBLELINE, first_line);
	undo.push_msg(SCI_SETDOCPOINTER, 0, (sptr_t)doc);
}

void
Document::update(void)
{
	anchor = interface.ssm(SCI_GETANCHOR);
	dot = interface.ssm(SCI_GETCURRENTPOS);
	first_line = interface.ssm(SCI_GETFIRSTVISIBLELINE);
	xoffset = interface.ssm(SCI_GETXOFFSET);
}

/*
 * Only for QRegisterStack::pop() which does some clever
 * exchanging of document data (without any deep copying)
 */
void
Document::exchange(Document &other)
{
	SciDoc temp_doc = doc;
	gint temp_anchor = anchor;
	gint temp_dot = dot;
	gint temp_first_line = first_line;
	gint temp_xoffset = xoffset;

	doc = other.doc;
	anchor = other.anchor;
	dot = other.dot;
	first_line = other.first_line;
	xoffset = other.xoffset;

	other.doc = temp_doc;
	other.anchor = temp_anchor;
	other.dot = temp_dot;
	other.first_line = temp_first_line;
	other.xoffset = temp_xoffset;
}

} /* namespace SciTECO */
