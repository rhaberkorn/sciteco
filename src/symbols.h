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

#ifndef __SYMBOLS_H
#define __SYMBOLS_H

#include <string.h>
#include <glib.h>

namespace SciTECO {

class SymbolList {
public:
	struct Entry {
		const gchar	*name;
		gint		value;
	};

private:
	const Entry	*entries;
	gint		size;
	int		(*cmp_fnc)(const char *, const char *, size_t);

	/* for auto-completions */
	GList		*list;

public:
	SymbolList(const Entry *_entries = NULL, gint _size = 0,
		   bool case_sensitive = false)
		  : entries(_entries), size(_size), list(NULL)
	{
		cmp_fnc = case_sensitive ? strncmp
					 : g_ascii_strncasecmp;
	}

	~SymbolList()
	{
		g_list_free(list);
	}

	gint lookup(const gchar *name, const gchar *prefix = "");
	GList *get_glist(void);
};

/* objects declared in symbols-minimal.cpp or auto-generated code */
namespace Symbols {
	extern SymbolList scintilla;
	extern SymbolList scilexer;
}

} /* namespace SciTECO */

#endif
