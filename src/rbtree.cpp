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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include <glib.h>
#include <glib/gprintf.h>

#include "sciteco.h"
#include "rbtree.h"
#include "interface.h"
#include "string-utils.h"

namespace SciTECO {

template <StringCmpFunc StringCmp, StringNCmpFunc StringNCmp>
gchar *
RBTreeStringT<StringCmp, StringNCmp>::
auto_complete(const gchar *key, gchar completed, gsize restrict_len)
{
	gsize key_len;
	RBEntryString *first = NULL;
	gsize prefix_len = 0;
	gint prefixed_entries = 0;
	gchar *insert = NULL;

	if (!key)
		key = "";
	key_len = strlen(key);

	for (RBEntryString *cur = nfind(key);
	     cur && !StringNCmp(cur->key, key, key_len);
	     cur = (RBEntryString *)cur->next()) {
		if (restrict_len && strlen(cur->key) != restrict_len)
			continue;

		if (!first)
			first = cur;

		gsize len = String::diff(first->key + key_len,
					 cur->key + key_len);
		if (!prefix_len || len < prefix_len)
			prefix_len = len;

		prefixed_entries++;
	}
	if (prefix_len > 0)
		insert = g_strndup(first->key + key_len, prefix_len);

	if (!insert && prefixed_entries > 1) {
		for (RBEntryString *cur = first;
		     cur && !StringNCmp(cur->key, key, key_len);
		     cur = (RBEntryString *)cur->next()) {
			if (restrict_len && strlen(cur->key) != restrict_len)
				continue;

			interface.popup_add(InterfaceCurrent::POPUP_PLAIN,
					    cur->key);
		}

		interface.popup_show();
	} else if (prefixed_entries == 1) {
		String::append(insert, completed);
	}

	return insert;
}

template class RBTreeStringT<strcmp, strncmp>;
template class RBTreeStringT<g_ascii_strcasecmp, g_ascii_strncasecmp>;

} /* namespace SciTECO */
