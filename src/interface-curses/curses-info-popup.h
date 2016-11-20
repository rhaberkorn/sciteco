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

#ifndef __CURSES_INFO_POPUP_H
#define __CURSES_INFO_POPUP_H

#include <glib.h>

#include <curses.h>

#include "memory.h"

namespace SciTECO {

class CursesInfoPopup : public Object {
public:
	/**
	 * @bug This is identical to the type defined in
	 *      interface.h. But for the sake of abstraction
	 *      we cannot access it here (or in gtk-info-popup
	 *      for that matter).
	 */
	enum PopupEntryType {
		POPUP_PLAIN,
		POPUP_FILE,
		POPUP_DIRECTORY
        };

private:
	WINDOW *window;		/**! window showing part of pad */
	WINDOW *pad;		/**! full-height entry list */

	struct Entry {
		PopupEntryType type;
		bool highlight;
		gchar name[];
	};

	GSList *list;		/**! list of popup entries */
	gint longest;		/**! size of longest entry */
	gint length;		/**! total number of popup entries */

	gint pad_first_line;	/**! first line in pad to show */

public:
	CursesInfoPopup() : window(NULL), pad(NULL),
	                    list(NULL), longest(0), length(0),
	                    pad_first_line(0) {}

	void add(PopupEntryType type,
		 const gchar *name, bool highlight = false);

	void show(attr_t attr);
	inline bool
	is_shown(void)
	{
		return window != NULL;
	}

	void clear(void);

	inline void
	noutrefresh(void)
	{
		if (window)
			wnoutrefresh(window);
	}

	~CursesInfoPopup();

private:
	void init_pad(attr_t attr);
};

} /* namespace SciTECO */

#endif
