/*
 * Copyright (C) 2012-2013 Robin Haberkorn
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

#ifndef __SYMBOLS_H
#define __SYMBOLS_H

#include <glib.h>

class SymbolList {
public:
	struct Entry {
		const gchar	*name;
		gint		value;
	};

	const Entry	*entries;
	gint		size;

private:
	/* for auto-completions */
	GList		*list;

public:
	SymbolList(const Entry *_entries = NULL, gint _size = 0)
		  : entries(_entries), size(_size), list(NULL) {}
	~SymbolList()
	{
		g_list_free(list);
	}

	gint lookup(const gchar *name, const gchar *prefix = "",
		    bool case_sensitive = false);
	GList *get_glist(void);
};

namespace Symbols {
	extern SymbolList scintilla;
	extern SymbolList scilexer;
}

#endif