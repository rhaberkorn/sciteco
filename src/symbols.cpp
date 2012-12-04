#include <string.h>

#include <glib.h>

#include "symbols.h"

/*
 * defaults for sciteco-minimal
 */
namespace Symbols {
	SymbolList __attribute__((weak)) scintilla;
	SymbolList __attribute__((weak)) scilexer;
}

/*
 * Since symbol lists are presorted constant arrays we can do a simple
 * binary search.
 */
gint
SymbolList::lookup(const gchar *name, const gchar *prefix, bool case_sensitive)
{
	int (*cmp_fnc)(const char *, const char *, size_t);
	gint prefix_skip = strlen(prefix);
	gint name_len = strlen(name);

	gint left = 0;
	gint right = size - 1;

	cmp_fnc = case_sensitive ? strncmp : g_ascii_strncasecmp;

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
