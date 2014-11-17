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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdarg.h>

#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>

#include <gdk/gdk.h>
#include <gdk/gdkkeysyms.h>

#include <gtk/gtk.h>
#include "gtk-info-popup.h"

#include <Scintilla.h>
#include <ScintillaWidget.h>

#include "sciteco.h"
#include "cmdline.h"
#include "qregisters.h"
#include "ring.h"
#include "interface.h"
#include "interface-gtk.h"

namespace SciTECO {

extern "C" {
static void scintilla_notify(ScintillaObject *sci, uptr_t idFrom,
			     SCNotification *notify, gpointer user_data);
static gboolean cmdline_key_pressed(GtkWidget *widget, GdkEventKey *event,
				    gpointer user_data);
static gboolean exit_app(GtkWidget *w, GdkEventAny *e, gpointer p);
}

#define UNNAMED_FILE "(Unnamed)"

ViewGtk::ViewGtk()
{
	sci = SCINTILLA(scintilla_new());
	/*
	 * We don't want the object to be destroyed
	 * when it is removed from the vbox.
	 */
	g_object_ref_sink(G_OBJECT(sci));

	scintilla_set_id(sci, 0);

	gtk_widget_set_usize(get_widget(), 500, 300);
	gtk_widget_set_can_focus(get_widget(), FALSE);

	g_signal_connect(G_OBJECT(sci), SCINTILLA_NOTIFY,
			 G_CALLBACK(scintilla_notify), NULL);

	initialize();
}

ViewGtk::~ViewGtk()
{
	/*
	 * This does NOT destroy the Scintilla object
	 * and GTK widget, if it is the current view
	 * (and therefore added to the vbox).
	 */
	g_object_unref(G_OBJECT(sci));
}

void
InterfaceGtk::main_impl(int &argc, char **&argv)
{
	GtkWidget *info_content;

	gtk_init(&argc, &argv);

	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(window), PACKAGE_NAME);
	g_signal_connect(G_OBJECT(window), "delete-event",
			 G_CALLBACK(exit_app), NULL);

	vbox = gtk_vbox_new(FALSE, 0);

	cmdline_widget = gtk_entry_new();
	gtk_entry_set_has_frame(GTK_ENTRY(cmdline_widget), FALSE);
	gtk_editable_set_editable(GTK_EDITABLE(cmdline_widget), FALSE);
	widget_set_font(cmdline_widget, "Courier");
	g_signal_connect(G_OBJECT(cmdline_widget), "key-press-event",
			 G_CALLBACK(cmdline_key_pressed), NULL);
	gtk_box_pack_end(GTK_BOX(vbox), cmdline_widget, FALSE, FALSE, 0);

	info_widget = gtk_info_bar_new();
	info_content = gtk_info_bar_get_content_area(GTK_INFO_BAR(info_widget));
	message_widget = gtk_label_new("");
	gtk_misc_set_alignment(GTK_MISC(message_widget), 0., 0.);
	gtk_container_add(GTK_CONTAINER(info_content), message_widget);
	gtk_box_pack_end(GTK_BOX(vbox), info_widget, FALSE, FALSE, 0);

	gtk_container_add(GTK_CONTAINER(window), vbox);

	popup_widget = gtk_info_popup_new(cmdline_widget);

	gtk_widget_grab_focus(cmdline_widget);

	cmdline_update("");
}

void
InterfaceGtk::vmsg_impl(MessageType type, const gchar *fmt, va_list ap)
{
	static const GtkMessageType type2gtk[] = {
		/* [MSG_USER]		= */ GTK_MESSAGE_OTHER,
		/* [MSG_INFO]		= */ GTK_MESSAGE_INFO,
		/* [MSG_WARNING]	= */ GTK_MESSAGE_WARNING,
		/* [MSG_ERROR]		= */ GTK_MESSAGE_ERROR
	};

	va_list aq;
	gchar buf[255];

	va_copy(aq, ap);
	stdio_vmsg(type, fmt, ap);
	g_vsnprintf(buf, sizeof(buf), fmt, aq);
	va_end(aq);

	gtk_info_bar_set_message_type(GTK_INFO_BAR(info_widget),
				      type2gtk[type]);
	gtk_label_set_text(GTK_LABEL(message_widget), buf);
}

void
InterfaceGtk::msg_clear(void)
{
	gtk_info_bar_set_message_type(GTK_INFO_BAR(info_widget),
				      GTK_MESSAGE_OTHER);
	gtk_label_set_text(GTK_LABEL(message_widget), "");
}

void
InterfaceGtk::show_view_impl(ViewGtk *view)
{
	/*
	 * The last view's object is not guaranteed to
	 * still exist.
	 * However its widget is, due to reference counting.
	 */
	if (current_view_widget)
		gtk_container_remove(GTK_CONTAINER(vbox),
		                     current_view_widget);

	current_view = view;
	current_view_widget = view->get_widget();

	gtk_box_pack_start(GTK_BOX(vbox), current_view_widget,
	                   TRUE, TRUE, 0);
	gtk_widget_show(current_view_widget);
}

void
InterfaceGtk::info_update_impl(QRegister *reg)
{
	gchar buf[255];

	g_snprintf(buf, sizeof(buf), "%s - <QRegister> %s", PACKAGE_NAME,
		   reg->name);
	gtk_window_set_title(GTK_WINDOW(window), buf);
}

void
InterfaceGtk::info_update_impl(Buffer *buffer)
{
	gchar buf[255];

	g_snprintf(buf, sizeof(buf), "%s - <Buffer> %s%s", PACKAGE_NAME,
		   buffer->filename ? : UNNAMED_FILE,
		   buffer->dirty ? "*" : "");
	gtk_window_set_title(GTK_WINDOW(window), buf);
}

void
InterfaceGtk::cmdline_update_impl(const gchar *cmdline)
{
	gint pos = 1;

	if (!cmdline)
		/* widget automatically redrawn */
		return;

	gtk_entry_set_text(GTK_ENTRY(cmdline_widget), "*");
	gtk_editable_insert_text(GTK_EDITABLE(cmdline_widget),
				 cmdline, -1, &pos);
	gtk_editable_set_position(GTK_EDITABLE(cmdline_widget), pos);
}

void
InterfaceGtk::popup_add_impl(PopupEntryType type,
                             const gchar *name, bool highlight)
{
	static const GtkInfoPopupEntryType type2gtk[] = {
		/* [POPUP_PLAIN]	= */ GTK_INFO_POPUP_PLAIN,
		/* [POPUP_FILE]		= */ GTK_INFO_POPUP_FILE,
		/* [POPUP_DIRECTORY]	= */ GTK_INFO_POPUP_DIRECTORY
	};

	gtk_info_popup_add(GTK_INFO_POPUP(popup_widget),
			   type2gtk[type], name, highlight);
}

void
InterfaceGtk::popup_clear_impl(void)
{
	if (gtk_widget_get_visible(popup_widget)) {
		gtk_widget_hide(popup_widget);
		gtk_info_popup_clear(GTK_INFO_POPUP(popup_widget));
	}
}

void
InterfaceGtk::widget_set_font(GtkWidget *widget, const gchar *font_name)
{
	PangoFontDescription *font_desc;

	font_desc = pango_font_description_from_string(font_name);
	gtk_widget_modify_font(widget, font_desc);
	pango_font_description_free(font_desc);
}

InterfaceGtk::~InterfaceGtk()
{
	if (popup_widget)
		gtk_widget_destroy(popup_widget);
	if (window)
		gtk_widget_destroy(window);

	scintilla_release_resources();
}

/*
 * GTK+ callbacks
 */

static void
scintilla_notify(ScintillaObject *sci, uptr_t idFrom,
		 SCNotification *notify, gpointer user_data)
{
	interface.process_notify(notify);
}

static gboolean
cmdline_key_pressed(GtkWidget *widget, GdkEventKey *event,
		    gpointer user_data)
{
	bool is_shift	= event->state & GDK_SHIFT_MASK;
	bool is_ctl	= event->state & GDK_CONTROL_MASK;

#ifdef DEBUG
	g_printf("KEY \"%s\" (%d) SHIFT=%d CNTRL=%d\n",
		 event->string, *event->string,
		 event->state & GDK_SHIFT_MASK, event->state & GDK_CONTROL_MASK);
#endif

	switch (event->keyval) {
	case GDK_Escape:
		cmdline_keypress('\x1B');
		break;
	case GDK_BackSpace:
		cmdline_keypress('\b');
		break;
	case GDK_Tab:
		cmdline_keypress('\t');
		break;
	case GDK_Return:
		cmdline_keypress(get_eol());
		break;

	/*
	 * Function key macros
	 */
#define FN(KEY, MACRO) \
	case GDK_##KEY: cmdline_fnmacro(#MACRO); break
#define FNS(KEY, MACRO) \
	case GDK_##KEY: cmdline_fnmacro(is_shift ? "S" #MACRO : #MACRO); break
	FN(Down, DOWN); FN(Up, UP);
	FNS(Left, LEFT); FNS(Right, RIGHT);
	FN(KP_Down, DOWN); FN(KP_Up, UP);
	FNS(KP_Left, LEFT); FNS(KP_Right, RIGHT);
	FNS(Home, HOME);
	case GDK_F1...GDK_F35: {
		gchar macro_name[3+1];

		g_snprintf(macro_name, sizeof(macro_name),
			   "F%d", event->keyval - GDK_F1 + 1);
		cmdline_fnmacro(macro_name);
		break;
	}
	FNS(Delete, DC);
	FNS(Insert, IC);
	FN(Page_Down, NPAGE); FN(Page_Up, PPAGE);
	FNS(Print, PRINT);
	FN(KP_Home, A1); FN(KP_Prior, A3);
	FN(KP_Begin, B2);
	FN(KP_End, C1); FN(KP_Next, C3);
	FNS(End, END);
	FNS(Help, HELP);
#undef FNS
#undef FN

	/*
	 * Control keys and keys with printable representation
	 */
	default:
		gunichar u = gdk_keyval_to_unicode(event->keyval);

		if (u && g_unichar_to_utf8(u, NULL) == 1) {
			gchar key;

			g_unichar_to_utf8(u, &key);
			if (key > 0x7F)
				break;
			if (is_ctl)
				key = CTL_KEY(g_ascii_toupper(key));

			cmdline_keypress(key);
		}
	}

	return TRUE;
}

static gboolean
exit_app(GtkWidget *w, GdkEventAny *e, gpointer p)
{
	/*
	 * FIXME: should instead insert "(EX)" or similar
	 * Perhaps something like a "QUIT" function key macro
	 */
	gtk_main_quit();
	return TRUE;
}

} /* namespace SciTECO */
