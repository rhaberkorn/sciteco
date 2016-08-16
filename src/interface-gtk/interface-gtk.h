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

#ifndef __INTERFACE_GTK_H
#define __INTERFACE_GTK_H

#include <stdarg.h>

#include <glib.h>

/* FIXME: see interface-gtk.cpp */
#define GDK_DISABLE_DEPRECATION_WARNINGS
#include <gdk/gdk.h>
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

	~ViewGtk();

	inline GtkWidget *
	get_widget(void)
	{
		return GTK_WIDGET(sci);
	}

	/* implementation of View::ssm() */
	inline sptr_t
	ssm_impl(unsigned int iMessage, uptr_t wParam = 0, sptr_t lParam = 0)
	{
		sptr_t ret;

		gdk_threads_enter();
		ret = scintilla_send_message(sci, iMessage, wParam, lParam);
		gdk_threads_leave();

		return ret;
	}
} ViewCurrent;

typedef class InterfaceGtk : public Interface<InterfaceGtk, ViewGtk> {
	GtkCssProvider *css_var_provider;

	GtkWidget *window;

	enum {
		INFO_TYPE_BUFFER = 0,
		INFO_TYPE_BUFFER_DIRTY,
		INFO_TYPE_QREGISTER
	} info_type;
	gchar *info_current;
	gboolean use_csd;
	GtkWidget *info_bar_widget;
	GtkWidget *info_image;
	GtkWidget *info_type_widget;
	GtkWidget *info_name_widget;

	GtkWidget *event_box_widget;

	GtkWidget *message_bar_widget;
	GtkWidget *message_widget;

	GtkWidget *cmdline_widget;

	GtkWidget *popup_widget;

	GtkWidget *current_view_widget;

	GAsyncQueue *event_queue;

public:
	InterfaceGtk() : css_var_provider(NULL),
	                 window(NULL),
	                 info_type(INFO_TYPE_BUFFER), info_current(NULL),
	                 use_csd(TRUE),
	                 info_bar_widget(NULL),
	                 info_image(NULL), info_type_widget(NULL), info_name_widget(NULL),
	                 event_box_widget(NULL),
	                 message_bar_widget(NULL), message_widget(NULL),
			 cmdline_widget(NULL),
			 popup_widget(NULL),
	                 current_view_widget(NULL),
	                 event_queue(NULL) {}
	~InterfaceGtk();

	/* overrides Interface::get_options() */
	GOptionGroup *get_options(void);

	/* implementation of Interface::main() */
	void main_impl(int &argc, char **&argv);

	/* implementation of Interface::vmsg() */
	void vmsg_impl(MessageType type, const gchar *fmt, va_list ap);
	/* overrides Interface::msg_clear() */
	void msg_clear(void);

	/* implementation of Interface::show_view() */
	void show_view_impl(ViewGtk *view);

	/* implementation of Interface::info_update() */
	void info_update_impl(const QRegister *reg);
	void info_update_impl(const Buffer *buffer);

	/* implementation of Interface::cmdline_update() */
	void cmdline_update_impl(const Cmdline *cmdline);

	/* override of Interface::set_clipboard() */
	void set_clipboard(const gchar *name,
	                   const gchar *str = NULL, gssize str_len = -1);
	/* override of Interface::get_clipboard() */
	gchar *get_clipboard(const gchar *name, gsize *str_len = NULL);

	/* implementation of Interface::popup_add() */
	void popup_add_impl(PopupEntryType type,
		            const gchar *name, bool highlight = false);
	/* implementation of Interface::popup_show() */
	void popup_show_impl(void);

	/* implementation of Interface::popup_is_shown() */
	inline bool
	popup_is_shown_impl(void)
	{
		bool ret;

		gdk_threads_enter();
		ret = gtk_widget_get_visible(popup_widget);
		gdk_threads_leave();

		return ret;
	}
	/* implementation of Interface::popup_clear() */
	void popup_clear_impl(void);

	/* main entry point (implementation) */
	void event_loop_impl(void);

	/*
	 * FIXME: This is for internal use only and could be
	 * hidden in a nested forward-declared friend struct.
	 */
	void handle_key_press(bool is_shift, bool is_ctl, guint keyval);

private:
	void set_css_variables_from_view(ViewGtk *view);

	void refresh_info(void);
	void cmdline_insert_chr(gint &pos, gchar chr);
} InterfaceCurrent;

} /* namespace SciTECO */

#endif
