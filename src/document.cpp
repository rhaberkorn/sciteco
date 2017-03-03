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

void
Document::edit(ViewCurrent &view)
{
	/*
	 * FIXME: SCI_SETREPRESENTATION does not redraw
	 * the screen - also that would be very slow.
	 * Since SCI_SETDOCPOINTER resets the representation
	 * (this should probably be fixed in Scintilla),
	 * the screen is garbled since the layout cache
	 * is calculated with the default representations.
	 * We work around this by temporarily disabling the
	 * layout cache.
	 */
	gint old_mode = view.ssm(SCI_GETLAYOUTCACHE);

	maybe_create_document();

	view.ssm(SCI_SETLAYOUTCACHE, SC_CACHE_NONE);

	view.ssm(SCI_SETDOCPOINTER, 0, (sptr_t)doc);
	view.ssm(SCI_SETFIRSTVISIBLELINE, first_line);
	view.ssm(SCI_SETXOFFSET, xoffset);
	view.ssm(SCI_SETSEL, anchor, (sptr_t)dot);

	/*
	 * Default TECO-style character representations.
	 * They are reset on EVERY SETDOCPOINTER call by Scintilla.
	 */
	view.set_representations();

	view.ssm(SCI_SETLAYOUTCACHE, old_mode);
}

void
Document::undo_edit(ViewCurrent &view)
{
	maybe_create_document();

	/*
	 * FIXME: see above in Document::edit()
	 */
	view.undo_ssm(SCI_SETLAYOUTCACHE,
	              view.ssm(SCI_GETLAYOUTCACHE));

	view.undo_set_representations();

	view.undo_ssm(SCI_SETSEL, anchor, (sptr_t)dot);
	view.undo_ssm(SCI_SETXOFFSET, xoffset);
	view.undo_ssm(SCI_SETFIRSTVISIBLELINE, first_line);
	view.undo_ssm(SCI_SETDOCPOINTER, 0, (sptr_t)doc);

	view.undo_ssm(SCI_SETLAYOUTCACHE, SC_CACHE_NONE);
}

void
Document::update(ViewCurrent &view)
{
	anchor = view.ssm(SCI_GETANCHOR);
	dot = view.ssm(SCI_GETCURRENTPOS);
	first_line = view.ssm(SCI_GETFIRSTVISIBLELINE);
	xoffset = view.ssm(SCI_GETXOFFSET);
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
