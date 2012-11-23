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

public:
	SymbolList(const Entry *_entries = NULL, gint _size = 0)
		  : entries(_entries), size(_size) {}

	gint lookup(const gchar *name);
	GList *get_glist(void);
};

namespace Symbols {
	extern SymbolList __attribute__((weak)) scintilla;
}

#endif
