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

#ifndef __INTERFACE_CURSES_H
#define __INTERFACE_CURSES_H

#include <stdarg.h>

#include <glib.h>

#include <curses.h>

#include <Scintilla.h>
#include <ScintillaTerm.h>

#include "interface.h"

namespace SciTECO {

typedef class ViewCurses : public View<ViewCurses> {
	Scintilla *sci;

public:
	ViewCurses() : sci(NULL) {}

	/* implementation of View::initialize() */
	void initialize_impl(void);

	inline ~ViewCurses()
	{
		/*
		 * NOTE: This deletes/frees the view's
		 * curses WINDOW, despite of what Scinterm's
		 * documentation says.
		 */
		if (sci)
			scintilla_delete(sci);
	}

	inline void
	refresh(void)
	{
		scintilla_refresh(sci);
	}

	inline WINDOW *
	get_window(void)
	{
		return scintilla_get_window(sci);
	}

	/* implementation of View::ssm() */
	inline sptr_t
	ssm_impl(unsigned int iMessage, uptr_t wParam = 0, sptr_t lParam = 0)
	{
		return scintilla_send_message(sci, iMessage, wParam, lParam);
	}
} ViewCurrent;

typedef class InterfaceCurses : public Interface<InterfaceCurses, ViewCurses> {
	SCREEN *screen;
	FILE *screen_tty;

	WINDOW *info_window;
	gchar *info_current;

	WINDOW *msg_window;

	WINDOW *cmdline_window;
	chtype *cmdline_current;
	gsize cmdline_len, cmdline_rubout_len;

	struct Popup {
		WINDOW *window;
		GSList *list;		/**! list of popup entries */
		gint longest;		/**! size of longest entry */
		gint length;		/**! total number of popup entries */

		GSList *cur_list;	/**! next entry to display */
		gint cur_entry;	/**! next entry to display (position) */

		Popup() : window(NULL), list(NULL),
		          longest(3), length(0),
		          cur_list(NULL), cur_entry(0) {}
		~Popup();
	} popup;

public:
	InterfaceCurses() : Interface(),
	                    screen(NULL),
			    screen_tty(NULL),
			    info_window(NULL),
			    info_current(NULL),
			    msg_window(NULL),
			    cmdline_window(NULL),
			    cmdline_current(NULL),
	                    cmdline_len(0), cmdline_rubout_len(0) {}
	~InterfaceCurses();

	/* implementation of Interface::main() */
	void main_impl(int &argc, char **&argv);

	/* implementation of Interface::vmsg() */
	void vmsg_impl(MessageType type, const gchar *fmt, va_list ap);
	/* override of Interface::msg_clear() */
	void msg_clear(void);

	/* implementation of Interface::show_view() */
	void show_view_impl(ViewCurses *view);

	/* implementation of Interface::info_update() */
	void info_update_impl(const QRegister *reg);
	void info_update_impl(const Buffer *buffer);

	/* implementation of Interface::cmdline_update() */
	void cmdline_update_impl(const Cmdline *cmdline);

	/* implementation of Interface::popup_add() */
	void popup_add_impl(PopupEntryType type,
		            const gchar *name, bool highlight = false);
	/* implementation of Interface::popup_show() */
	void popup_show_impl(void);
	/* implementation of Interface::popup_is_shown() */
	inline bool
	popup_is_shown_impl(void)
	{
		return popup.window != NULL;
	}
	/* implementation of Interface::popup_clear() */
	void popup_clear_impl(void);

	/* main entry point (implementation) */
	void event_loop_impl(void);

private:
	void init_batch(void);
	void init_interactive(void);

	void resize_all_windows(void);

	void set_window_title(const gchar *title);
	void draw_info(void);

	void format_chr(chtype *&target, gchar chr,
	                attr_t attr = 0);
	void draw_cmdline(void);

	friend void event_loop_iter();
} InterfaceCurrent;

} /* namespace SciTECO */

#endif
