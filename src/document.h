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

#ifndef __DOCUMENT_H
#define __DOCUMENT_H

#include <glib.h>

#include <Scintilla.h>

#include "sciteco.h"
#include "interface.h"
#include "undo.h"

namespace SciTECO {

/*
 * Classes
 */

class Document {
	typedef const void *SciDoc;
	SciDoc doc;

	/* updated/restored only when required */
	gint anchor, dot;
	gint first_line, xoffset;

public:
	Document() : doc(NULL)
	{
		reset();
	}
	virtual ~Document()
	{
		if (is_initialized())
			interface.ssm(SCI_RELEASEDOCUMENT, 0, (sptr_t)doc);
	}

	inline bool
	is_initialized(void)
	{
		return doc != NULL;
	}

	void edit(void);
	void undo_edit(void);

	void update(void);
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
};

} /* namespace SciTECO */

#endif
