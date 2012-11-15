#ifndef __INTERFACE_NCURSES_H
#define __INTERFACE_NCURSES_H

#include <stdarg.h>

#include <glib.h>

#include <ncurses.h>

#include <Scintilla.h>
#include <ScintillaTerm.h>

#include "interface.h"

extern class InterfaceNCurses : public Interface {
	SCREEN *screen;
	FILE *screen_tty;

	Scintilla *sci;

	WINDOW *sci_window;
	WINDOW *msg_window;
	WINDOW *cmdline_window;

	WINDOW *popup_window;
	GSList *popup_list;
	gint popup_list_longest;
	gint popup_list_length;

public:
	InterfaceNCurses();
	~InterfaceNCurses();

	void vmsg(MessageType type, const gchar *fmt, va_list ap);

	inline sptr_t
	ssm(unsigned int iMessage, uptr_t wParam = 0, sptr_t lParam = 0)
	{
		return scintilla_send_message(sci, iMessage, wParam, lParam);
	}

	void cmdline_update(const gchar *cmdline = "");

	void popup_add_filename(PopupFileType type,
				const gchar *filename, bool highlight = false);
	void popup_show(void);
	void popup_clear(void);

	/* main entry point */
	void event_loop(void);
} interface;

#endif
