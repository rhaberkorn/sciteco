/*
 * Copyright (C) 2012-2013 Robin Haberkorn
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

#ifndef __INTERFACE_GTK_H
#define __INTERFACE_GTK_H

#include <stdarg.h>

#include <glib.h>
#include <gtk/gtk.h>

#include <Scintilla.h>
#include <ScintillaWidget.h>

#include "interface.h"

extern class InterfaceGtk : public Interface {
	GtkWidget *window;
	GtkWidget *editor_widget;
	GtkWidget *cmdline_widget;
	GtkWidget *info_widget, *message_widget;

	GtkWidget *popup_widget;

public:
	InterfaceGtk();
	~InterfaceGtk();

	inline GOptionGroup *
	get_options(void)
	{
		return gtk_get_option_group(TRUE);
	}
	inline void
	parse_args(int &argc, char **&argv)
	{
		gtk_parse_args(&argc, &argv);
	}

	void vmsg(MessageType type, const gchar *fmt, va_list ap);
	void msg_clear(void);

	inline sptr_t
	ssm(unsigned int iMessage, uptr_t wParam = 0, sptr_t lParam = 0)
	{
		return scintilla_send_message(SCINTILLA(editor_widget),
					      iMessage, wParam, lParam);
	}

	void info_update(QRegister *reg);
	void info_update(Buffer *buffer);

	void cmdline_update(const gchar *cmdline = NULL);

	void popup_add(PopupEntryType type,
		       const gchar *name, bool highlight = false);
	inline void
	popup_show(void)
	{
		gtk_widget_show(popup_widget);
	}
	void popup_clear(void);

	/* main entry point */
	inline void
	event_loop(void)
	{
		gtk_widget_show_all(window);
		gtk_main();
	}

private:
	static void widget_set_font(GtkWidget *widget, const gchar *font_name);
} interface;

#endif
