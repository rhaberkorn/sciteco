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

#ifndef __DOCUMENT_H
#define __DOCUMENT_H

#include <glib.h>

#include <Scintilla.h>

#include "sciteco.h"
#include "memory.h"
#include "interface.h"
#include "undo.h"

namespace SciTECO {

/*
 * Classes
 */

class Document : public Object {
	typedef const void *SciDoc;
	SciDoc doc;

	/*
	 * The so called "parameters".
	 * Updated/restored only when required
	 */
	gint anchor, dot;
	gint first_line, xoffset;

public:
	Document() : doc(NULL)
	{
		reset();
	}
	virtual ~Document()
	{
		/*
		 * Cannot release document here, since we must
		 * do it on the same view that created it.
		 * We also cannot call get_create_document_view()
		 * since it is virtual.
		 * So we must demand that deriving classes call
		 * release_document() from their destructors.
		 */
		g_assert(doc == NULL);
	}

	inline bool
	is_initialized(void)
	{
		return doc != NULL;
	}

	void edit(ViewCurrent &view);
	void undo_edit(ViewCurrent &view);

	void update(ViewCurrent &view);
	inline void
	update(const Document &from)
	{
		anchor = from.anchor;
		dot = from.dot;
		first_line = from.first_line;
		xoffset = from.xoffset;
	}

	inline void
	reset(void)
	{
		anchor = dot = 0;
		first_line = xoffset = 0;
	}
	inline void
	undo_reset(void)
	{
		undo.push_var(anchor);
		undo.push_var(dot);
		undo.push_var(first_line);
		undo.push_var(xoffset);
	}

	void exchange(Document &other);
	inline void
	undo_exchange(void)
	{
		undo.push_var(doc);
		undo_reset();
	}

protected:
	inline void
	release_document(void)
	{
		if (is_initialized()) {
			ViewCurrent &view = get_create_document_view();
			view.ssm(SCI_RELEASEDOCUMENT, 0, (sptr_t)doc);
			doc = NULL;
		}
	}

private:
	/*
	 * Must be implemented by derived class.
	 * Documents must be released on the same view
	 * as they were created.
	 * Since we do not want to save this view
	 * per document, it must instead be returned by
	 * this method.
	 */
	virtual ViewCurrent &get_create_document_view(void) = 0;

	inline void
	maybe_create_document(void)
	{
		if (!is_initialized()) {
			ViewCurrent &view = get_create_document_view();
			doc = (SciDoc)view.ssm(SCI_CREATEDOCUMENT);
		}
	}
};

} /* namespace SciTECO */

#endif
