#ifndef __INTERFACE_NCURSES_H
#define __INTERFACE_NCURSES_H

#include <stdarg.h>

#include <glib.h>

#include <curses.h>

#include <Scintilla.h>
#include <ScintillaTerm.h>

#include "interface.h"

extern class InterfaceNCurses : public Interface {
	SCREEN *screen;
	FILE *screen_tty;

	Scintilla *sci;

	WINDOW *info_window;
	gchar *info_current;
	WINDOW *sci_window;
	WINDOW *msg_window;
	WINDOW *cmdline_window;
	gchar *cmdline_current;

	struct Popup {
		WINDOW *window;
		GSList *list;
		gint longest;
		gint length;

		Popup() : window(NULL), list(NULL), longest(0), length(0) {}
		~Popup();
	} popup;

	void init_screen(void);
	void resize_all_windows(void);
	void draw_info(void);

public:
	InterfaceNCurses();
	~InterfaceNCurses();

	void vmsg(MessageType type, const gchar *fmt, va_list ap);
	void msg_clear(void);

	inline sptr_t
	ssm(unsigned int iMessage, uptr_t wParam = 0, sptr_t lParam = 0)
	{
		return scintilla_send_message(sci, iMessage, wParam, lParam);
	}

	void info_update(QRegister *reg);
	void info_update(Buffer *buffer);

	void cmdline_update(const gchar *cmdline = NULL);

	void popup_add(PopupEntryType type,
		       const gchar *name, bool highlight = false);
	void popup_show(void);
	void popup_clear(void);

	/* main entry point */
	void event_loop(void);
} interface;

#endif
