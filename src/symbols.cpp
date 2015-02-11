/*
 * Copyright (C) 2012-2015 Robin Haberkorn
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

#include <string.h>

#include <glib.h>

#include "sciteco.h"
#include "symbols.h"

namespace SciTECO {

/*
 * Since symbol lists are presorted constant arrays we can do a simple
 * binary search.
 */
gint
SymbolList::lookup(const gchar *name, const gchar *prefix)
{
	gint prefix_skip = strlen(prefix);
	gint name_len = strlen(name);

	gint left = 0;
	gint right = size - 1;

	if (!cmp_fnc(name, prefix, prefix_skip))
		prefix_skip = 0;

	while (left <= right) {
		gint cur = left + (right-left)/2;
		gint cmp = cmp_fnc(entries[cur].name + prefix_skip,
				   name, name_len + 1);

		if (!cmp)
			return entries[cur].value;

		if (cmp > 0)
			right = cur-1;
		else /* cmp < 0 */
			left = cur+1;
	}

	return -1;
}

GList *
SymbolList::get_glist(void)
{
	if (!list) {
		for (gint i = size; i; i--)
			list = g_list_prepend(list, (gchar *)entries[i-1].name);
	}

	return list;
}

} /* namespace SciTECO */
