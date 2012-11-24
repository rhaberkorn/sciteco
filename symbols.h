#ifndef __SYMBOLS_H
#define __SYMBOLS_H

#include <glib.h>

class SymbolList {
public:
	struct Entry {
		const gchar	*name;
		gint		value;
	};

private:
	const Entry	*entries;
	gint		size;

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
	extern SymbolList __attribute__((weak)) scintilla;
	extern SymbolList __attribute__((weak)) scilexer;
}

#endif
