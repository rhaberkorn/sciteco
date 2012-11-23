#include <glib.h>

#include "symbols.h"

/*
 * defaults for sciteco-minimal
 */
namespace Symbols {
	SymbolList __attribute__((weak)) scintilla;
}

/*
 * Since symbol lists are presorted constant arrays we can do a simple
 * binary search.
 */
gint
SymbolList::lookup(const gchar *name)
{
	gint left = 0;
	gint right = size - 1;

	while (left <= right) {
		gint cur = left + (right-left)/2;
		gint cmp = g_strcmp0(entries[cur].name, name);

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
	GList *list = NULL;

	while (size--)
		list = g_list_prepend(list, (gchar *)entries[size].name);

	return list;
}
