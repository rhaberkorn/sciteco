/*
 * Copyright (C) 2012-2014 Robin Haberkorn
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

namespace SciTECO {

typedef class ViewGtk : public View<ViewGtk> {
	ScintillaObject *sci;

public:
	ViewGtk() : sci(NULL) {}

	/* implementation of View::initialize() */
	void initialize_impl(void);

	inline ~ViewGtk()
	{
		/*
		 * This does NOT destroy the Scintilla object
		 * and GTK widget, if it is the current view
		 * (and therefore added to the vbox).
		 */
		if (sci)
			g_object_unref(G_OBJECT(sci));
	}

	inline GtkWidget *
	get_widget(void)
	{
		return GTK_WIDGET(sci);
	}

	/* implementation of View::ssm() */
	inline sptr_t
	ssm_impl(unsigned int iMessage, uptr_t wParam = 0, sptr_t lParam = 0)
	{
		return scintilla_send_message(sci, iMessage, wParam, lParam);
	}
} ViewCurrent;

typedef class InterfaceGtk : public Interface<InterfaceGtk, ViewGtk> {
	GtkWidget *window;
	GtkWidget *vbox;
	GtkWidget *cmdline_widget;
	GtkWidget *info_widget, *message_widget;

	GtkWidget *popup_widget;

	GtkWidget *current_view_widget;

public:
	InterfaceGtk() : Interface(),
	                 window(NULL),
	                 vbox(NULL),
			 cmdline_widget(NULL),
			 info_widget(NULL), message_widget(NULL),
			 popup_widget(NULL),
	                 current_view_widget(NULL) {}
	~InterfaceGtk();

	/* overrides Interface::get_options() */
	inline GOptionGroup *
	get_options(void)
	{
		return gtk_get_option_group(TRUE);
	}

	/* implementation of Interface::main() */
	void main_impl(int &argc, char **&argv);

	/* implementation of Interface::vmsg() */
	void vmsg_impl(MessageType type, const gchar *fmt, va_list ap);
	/* overrides Interface::msg_clear() */
	void msg_clear(void);

	/* implementation of Interface::show_view() */
	void show_view_impl(ViewGtk *view);

	/* implementation of Interface::info_update() */
	void info_update_impl(QRegister *reg);
	void info_update_impl(Buffer *buffer);

	/* implementation of Interface::cmdline_update() */
	void cmdline_update_impl(const gchar *cmdline = NULL);

	/* implementation of Interface::popup_add() */
	void popup_add_impl(PopupEntryType type,
		            const gchar *name, bool highlight = false);
	/* implementation of Interface::popup_show() */
	inline void
	popup_show_impl(void)
	{
		/* FIXME: scroll through popup contents */
		gtk_widget_show(popup_widget);
	}
	/* implementation of Interface::popup_is_shown() */
	inline bool
	popup_is_shown_impl(void)
	{
		return gtk_widget_get_visible(popup_widget);
	}
	/* implementation of Interface::popup_clear() */
	void popup_clear_impl(void);

	/* main entry point (implementation) */
	inline void
	event_loop_impl(void)
	{
		gtk_widget_show_all(window);
		gtk_main();
	}

private:
	static void widget_set_font(GtkWidget *widget, const gchar *font_name);
} InterfaceCurrent;

} /* namespace SciTECO */

#endif
