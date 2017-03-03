/*
 * Copyright (C) 2012-2017 Robin Haberkorn
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
#include <string.h>
#include <signal.h>

#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>

/*
 * FIXME: Because of gdk_threads_enter().
 * The only way to do it in Gtk3 style would be using
 * idle callbacks into the main thread and sync barriers (inefficient!)
 * or doing it single-threaded and ticking the Gtk main loop
 * (may be inefficient since gtk_events_pending() is doing
 * syscalls; however that may be ailed by doing it less frequently).
 */
#define GDK_DISABLE_DEPRECATION_WARNINGS
#include <gdk/gdk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include <gtk/gtk.h>

#include <gio/gio.h>

#include <Scintilla.h>
#include <ScintillaWidget.h>

#include "gtk-info-popup.h"
#include "gtk-canonicalized-label.h"

#include "sciteco.h"
#include "string-utils.h"
#include "cmdline.h"
#include "qregisters.h"
#include "ring.h"
#include "interface.h"
#include "interface-gtk.h"

/*
 * Signal handlers (e.g. for handling SIGTERM) are only
 * available on Unix and beginning with v2.30, while
 * we still support v2.28.
 * Handlers using `signal()` cannot be used easily for
 * this purpose.
 */
#if defined(G_OS_UNIX) && GLIB_CHECK_VERSION(2,30,0)
#include <glib-unix.h>
#define SCITECO_HANDLE_SIGNALS
#endif

namespace SciTECO {

extern "C" {

static void scintilla_notify(ScintillaObject *sci, uptr_t idFrom,
                             SCNotification *notify, gpointer user_data);

static gpointer exec_thread_cb(gpointer data);
static gboolean cmdline_key_pressed_cb(GtkWidget *widget, GdkEventKey *event,
                                       gpointer user_data);
static gboolean window_delete_cb(GtkWidget *w, GdkEventAny *e,
                                 gpointer user_data);

static gboolean sigterm_handler(gpointer user_data) G_GNUC_UNUSED;

static gboolean
g_object_unref_idle_cb(gpointer user_data)
{
	g_object_unref(user_data);
	return G_SOURCE_REMOVE;
}

} /* extern "C" */

#define UNNAMED_FILE		"(Unnamed)"

#define USER_CSS_FILE		".teco_css"

/** printf() format for CSS RGB colors given as guint32 */
#define CSS_COLOR_FORMAT	"#%06" G_GINT32_MODIFIER "X"

/**
 * Convert Scintilla-style BGR color triple to
 * RGB.
 */
static inline guint32
bgr2rgb(guint32 bgr)
{
	return ((bgr & 0x0000FF) << 16) |
	       ((bgr & 0x00FF00) << 0) |
	       ((bgr & 0xFF0000) >> 16);
}

void
ViewGtk::initialize_impl(void)
{
	gint events;

	gdk_threads_enter();

	sci = SCINTILLA(scintilla_new());
	/*
	 * We don't want the object to be destroyed
	 * when it is removed from the vbox.
	 */
	g_object_ref_sink(sci);

	scintilla_set_id(sci, 0);

	gtk_widget_set_size_request(get_widget(), 500, 300);

	/*
	 * This disables mouse and key events on this view.
	 * For some strange reason, masking events on
	 * the event box does NOT work.
	 * NOTE: Scroll events are still allowed - scrolling
	 * is currently not under direct control of SciTECO
	 * (i.e. it is OK the side effects of scrolling are not
	 * tracked).
	 */
	gtk_widget_set_can_focus(get_widget(), FALSE);
	events = gtk_widget_get_events(get_widget());
	events &= ~(GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
	events &= ~(GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK);
	gtk_widget_set_events(get_widget(), events);

	g_signal_connect(sci, SCINTILLA_NOTIFY,
			 G_CALLBACK(scintilla_notify), NULL);

	/*
	 * setup() calls Scintilla messages, so we must unlock
	 * here already to avoid deadlocks.
	 */
	gdk_threads_leave();

	setup();
}

ViewGtk::~ViewGtk()
{
	/*
	 * This does NOT destroy the Scintilla object
	 * and GTK widget, if it is the current view
	 * (and therefore added to the vbox).
	 * FIXME: This only uses an idle watcher
	 * because the destructor can be called with
	 * the Gdk lock held and without.
	 * Once the threading model is revised this
	 * can be simplified and inlined again.
	 */
	if (sci)
		gdk_threads_add_idle(g_object_unref_idle_cb, sci);
}

GOptionGroup *
InterfaceGtk::get_options(void)
{
	const GOptionEntry entries[] = {
		{"no-csd", 0, G_OPTION_FLAG_IN_MAIN | G_OPTION_FLAG_REVERSE,
	         G_OPTION_ARG_NONE, &use_csd,
		 "Disable client-side decorations.", NULL},
		{NULL}
	};

	/*
	 * Parsing the option context with the Gtk option group
	 * will automatically initialize Gtk, but we do not yet
	 * open the default display.
	 */
	GOptionGroup *group = gtk_get_option_group(FALSE);

	g_option_group_add_entries(group, entries);

	return group;
}

void
InterfaceGtk::init(void)
{
	static const Cmdline empty_cmdline;

	GtkWidget *vbox;
	GtkWidget *overlay_widget, *overlay_vbox;
	GtkWidget *message_bar_content;

	/*
	 * g_thread_init() is required prior to v2.32
	 * (we still support v2.28) but generates a warning
	 * on newer versions.
	 */
#if !GLIB_CHECK_VERSION(2,32,0)
	g_thread_init(NULL);
#endif
	gdk_threads_init();

	/*
	 * gtk_init() is not necessary when using gtk_get_option_group(),
	 * but this will open the default display.
	 * FIXME: Perhaps it is possible to defer this until we initialize
	 * interactive mode!?
	 */
	gtk_init(NULL, NULL);

	/*
	 * Register clipboard registers.
	 * Unfortunately, we cannot find out which
	 * clipboards/selections are supported on this system,
	 * so we register only some default ones.
	 */
	QRegisters::globals.insert(new QRegisterClipboard());
	QRegisters::globals.insert(new QRegisterClipboard("P"));
	QRegisters::globals.insert(new QRegisterClipboard("S"));
	QRegisters::globals.insert(new QRegisterClipboard("C"));

	/*
	 * The event queue is initialized now, so we can
	 * pass it as user data to C-linkage callbacks.
	 */
	event_queue = g_async_queue_new();

	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	g_signal_connect(G_OBJECT(window), "delete-event",
			 G_CALLBACK(window_delete_cb), event_queue);

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

	info_current = g_strdup("");

	/*
	 * The info bar is tried to be made the title bar of the
	 * window which also disables the default window decorations
	 * (client-side decorations) unless --no-csd was specified.
	 * NOTE: Client-side decoations could fail, leaving us with a
	 * standard title bar and the info bar with close buttons.
	 * Other window managers have undesirable side-effects.
	 */
	info_bar_widget = gtk_header_bar_new();
	gtk_widget_set_name(info_bar_widget, "sciteco-info-bar");
	info_name_widget = gtk_canonicalized_label_new(NULL);
	gtk_widget_set_valign(info_name_widget, GTK_ALIGN_CENTER);
	gtk_style_context_add_class(gtk_widget_get_style_context(info_name_widget),
	                            "name-label");
	gtk_label_set_selectable(GTK_LABEL(info_name_widget), TRUE);
	/* NOTE: Header bar does not resize for multi-line labels */
	//gtk_label_set_line_wrap(GTK_LABEL(info_name_widget), TRUE);
	//gtk_label_set_lines(GTK_LABEL(info_name_widget), 2);
	gtk_header_bar_set_custom_title(GTK_HEADER_BAR(info_bar_widget), info_name_widget);
	info_image = gtk_image_new();
	gtk_header_bar_pack_start(GTK_HEADER_BAR(info_bar_widget), info_image);
	info_type_widget = gtk_label_new(NULL);
	gtk_widget_set_valign(info_type_widget, GTK_ALIGN_CENTER);
	gtk_style_context_add_class(gtk_widget_get_style_context(info_type_widget),
	                            "type-label");
	gtk_header_bar_pack_start(GTK_HEADER_BAR(info_bar_widget), info_type_widget);
	if (use_csd) {
		/* use client-side decorations */
		gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(info_bar_widget), TRUE);
		gtk_window_set_titlebar(GTK_WINDOW(window), info_bar_widget);
	} else {
		/* fall back to adding the info bar as an ordinary widget */
		gtk_box_pack_start(GTK_BOX(vbox), info_bar_widget, FALSE, FALSE, 0);
	}

	/*
	 * Overlay widget will allow overlaying the Scintilla view
	 * and message widgets with the info popup.
	 * Therefore overlay_vbox (containing the view and popup)
	 * will be the main child of the overlay.
	 */
	overlay_widget = gtk_overlay_new();
	overlay_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

	/*
	 * The event box is the parent of all Scintilla views
	 * that should be displayed.
	 * This is handy when adding or removing current views,
	 * enabling and disabling GDK updates and in order to filter
	 * mouse and keyboard events going to Scintilla.
	 */
	event_box_widget = gtk_event_box_new();
	gtk_event_box_set_above_child(GTK_EVENT_BOX(event_box_widget), TRUE);
	gtk_box_pack_start(GTK_BOX(overlay_vbox), event_box_widget,
	                   TRUE, TRUE, 0);

	message_bar_widget = gtk_info_bar_new();
	gtk_widget_set_name(message_bar_widget, "sciteco-message-bar");
	message_bar_content = gtk_info_bar_get_content_area(GTK_INFO_BAR(message_bar_widget));
	/* NOTE: Messages are always pre-canonicalized */
	message_widget = gtk_label_new(NULL);
	gtk_label_set_selectable(GTK_LABEL(message_widget), TRUE);
	gtk_label_set_line_wrap(GTK_LABEL(message_widget), TRUE);
	gtk_container_add(GTK_CONTAINER(message_bar_content), message_widget);
	gtk_box_pack_start(GTK_BOX(overlay_vbox), message_bar_widget,
	                   FALSE, FALSE, 0);

	gtk_container_add(GTK_CONTAINER(overlay_widget), overlay_vbox);
	gtk_box_pack_start(GTK_BOX(vbox), overlay_widget, TRUE, TRUE, 0);

	cmdline_widget = gtk_entry_new();
	gtk_widget_set_name(cmdline_widget, "sciteco-cmdline");
	gtk_entry_set_has_frame(GTK_ENTRY(cmdline_widget), FALSE);
	gtk_editable_set_editable(GTK_EDITABLE(cmdline_widget), FALSE);
	g_signal_connect(G_OBJECT(cmdline_widget), "key-press-event",
			 G_CALLBACK(cmdline_key_pressed_cb), event_queue);
	gtk_box_pack_start(GTK_BOX(vbox), cmdline_widget, FALSE, FALSE, 0);

	gtk_container_add(GTK_CONTAINER(window), vbox);

	/*
	 * Popup widget will be shown in the bottom
	 * of the overlay widget (i.e. the Scintilla views),
	 * filling the entire width.
	 */
	popup_widget = gtk_info_popup_new();
	gtk_widget_set_name(popup_widget, "sciteco-info-popup");
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay_widget), popup_widget);
	g_signal_connect(overlay_widget, "get-child-position",
	                 G_CALLBACK(gtk_info_popup_get_position_in_overlay), NULL);

	gtk_widget_grab_focus(cmdline_widget);

	cmdline_update(&empty_cmdline);
}

void
InterfaceGtk::vmsg_impl(MessageType type, const gchar *fmt, va_list ap)
{
	/*
	 * The message types are chosen such that there is a CSS class
	 * for every one of them. GTK_MESSAGE_OTHER does not have
	 * a CSS class.
	 */
	static const GtkMessageType type2gtk[] = {
		/* [MSG_USER]		= */ GTK_MESSAGE_QUESTION,
		/* [MSG_INFO]		= */ GTK_MESSAGE_INFO,
		/* [MSG_WARNING]	= */ GTK_MESSAGE_WARNING,
		/* [MSG_ERROR]		= */ GTK_MESSAGE_ERROR
	};

	va_list aq;
	gchar buf[255];

	/*
	 * stdio_vmsg() leaves `ap` undefined and we are expected
	 * to do the same and behave like vprintf().
	 */
	va_copy(aq, ap);
	stdio_vmsg(type, fmt, ap);
	g_vsnprintf(buf, sizeof(buf), fmt, aq);
	va_end(aq);

	gdk_threads_enter();

	gtk_info_bar_set_message_type(GTK_INFO_BAR(message_bar_widget),
				      type2gtk[type]);
	gtk_label_set_text(GTK_LABEL(message_widget), buf);

	if (type == MSG_ERROR)
		gtk_widget_error_bell(window);

	gdk_threads_leave();
}

void
InterfaceGtk::msg_clear(void)
{
	gdk_threads_enter();

	gtk_info_bar_set_message_type(GTK_INFO_BAR(message_bar_widget),
				      GTK_MESSAGE_QUESTION);
	gtk_label_set_text(GTK_LABEL(message_widget), "");

	gdk_threads_leave();
}

void
InterfaceGtk::show_view_impl(ViewGtk *view)
{
	current_view = view;
}

void
InterfaceGtk::refresh_info(void)
{
	GtkStyleContext *style = gtk_widget_get_style_context(info_bar_widget);
	const gchar *info_type_str = PACKAGE;
	gchar *info_current_temp = g_strdup(info_current);
	gchar *info_current_canon;
	GIcon *icon;
	gchar *title;

	gtk_style_context_remove_class(style, "info-qregister");
	gtk_style_context_remove_class(style, "info-buffer");
	gtk_style_context_remove_class(style, "dirty");

	if (info_type == INFO_TYPE_BUFFER_DIRTY)
		String::append(info_current_temp, "*");
	gtk_canonicalized_label_set_text(GTK_CANONICALIZED_LABEL(info_name_widget),
	                                 info_current_temp);
	info_current_canon = String::canonicalize_ctl(info_current_temp);
	g_free(info_current_temp);

	switch (info_type) {
	case INFO_TYPE_QREGISTER:
		gtk_style_context_add_class(style, "info-qregister");

		info_type_str = PACKAGE_NAME " - <QRegister> ";
		gtk_label_set_text(GTK_LABEL(info_type_widget), "QRegister");
		gtk_label_set_ellipsize(GTK_LABEL(info_name_widget),
		                        PANGO_ELLIPSIZE_START);

		/* FIXME: Use a Q-Register icon */
		gtk_image_clear(GTK_IMAGE(info_image));
		break;

	case INFO_TYPE_BUFFER_DIRTY:
		gtk_style_context_add_class(style, "dirty");
		/* fall through */
	case INFO_TYPE_BUFFER:
		gtk_style_context_add_class(style, "info-buffer");

		info_type_str = PACKAGE_NAME " - <Buffer> ";
		gtk_label_set_text(GTK_LABEL(info_type_widget), "Buffer");
		gtk_label_set_ellipsize(GTK_LABEL(info_name_widget),
		                        PANGO_ELLIPSIZE_MIDDLE);

		icon = gtk_info_popup_get_icon_for_path(info_current,
		                                        "text-x-generic");
		if (!icon)
			break;
		gtk_image_set_from_gicon(GTK_IMAGE(info_image),
		                         icon, GTK_ICON_SIZE_LARGE_TOOLBAR);
		g_object_unref(icon);
		break;
	}

	title = g_strconcat(info_type_str, info_current_canon, NIL);
	gtk_window_set_title(GTK_WINDOW(window), title);
	g_free(title);
	g_free(info_current_canon);
}

void
InterfaceGtk::info_update_impl(const QRegister *reg)
{
	g_free(info_current);
	info_type = INFO_TYPE_QREGISTER;
	/* NOTE: will contain control characters */
	info_current = g_strdup(reg->name);
}

void
InterfaceGtk::info_update_impl(const Buffer *buffer)
{
	g_free(info_current);
	info_type = buffer->dirty ? INFO_TYPE_BUFFER_DIRTY
	                          : INFO_TYPE_BUFFER;
	info_current = g_strdup(buffer->filename ? : UNNAMED_FILE);
}

void
InterfaceGtk::cmdline_insert_chr(gint &pos, gchar chr)
{
	gchar buffer[5+1];

	/*
	 * NOTE: This mapping is similar to
	 * View::set_representations()
	 */
	switch (chr) {
	case CTL_KEY_ESC:
		strcpy(buffer, "$");
		break;
	case '\r':
		strcpy(buffer, "<CR>");
		break;
	case '\n':
		strcpy(buffer, "<LF>");
		break;
	case '\t':
		strcpy(buffer, "<TAB>");
		break;
	default:
		if (IS_CTL(chr)) {
			buffer[0] = '^';
			buffer[1] = CTL_ECHO(chr);
			buffer[2] = '\0';
		} else {
			buffer[0] = chr;
			buffer[1] = '\0';
		}
	}

	gtk_editable_insert_text(GTK_EDITABLE(cmdline_widget),
				 buffer, -1, &pos);
}

void
InterfaceGtk::cmdline_update_impl(const Cmdline *cmdline)
{
	gint pos = 1;
	gint cmdline_len;

	gdk_threads_enter();

	/*
	 * We don't know if the new command line is similar to
	 * the old one, so we can just as well rebuild it.
	 */
	gtk_entry_set_text(GTK_ENTRY(cmdline_widget), "*");

	/* format effective command line */
	for (guint i = 0; i < cmdline->len; i++)
		cmdline_insert_chr(pos, (*cmdline)[i]);
	/* save end of effective command line */
	cmdline_len = pos;

	/* format rubbed out command line */
	for (guint i = cmdline->len; i < cmdline->len+cmdline->rubout_len; i++)
		cmdline_insert_chr(pos, (*cmdline)[i]);

	/* set cursor after effective command line */
	gtk_editable_set_position(GTK_EDITABLE(cmdline_widget), cmdline_len);

	gdk_threads_leave();
}

static GdkAtom
get_selection_by_name(const gchar *name)
{
	/*
	 * We can use gdk_atom_intern() to support arbitrary X11 selection
	 * names. However, since we cannot find out which selections are
	 * registered, we are only providing QRegisters for the three default
	 * selections.
	 * Checking them here avoids expensive X server roundtrips.
	 */
	switch (*name) {
	case '\0': return GDK_NONE;
	case 'P':  return GDK_SELECTION_PRIMARY;
	case 'S':  return GDK_SELECTION_SECONDARY;
	case 'C':  return GDK_SELECTION_CLIPBOARD;
	default:   break;
	}

	return gdk_atom_intern(name, FALSE);
}

void
InterfaceGtk::set_clipboard(const gchar *name, const gchar *str, gssize str_len)
{
	GtkClipboard *clipboard;

	gdk_threads_enter();

	clipboard = gtk_clipboard_get(get_selection_by_name(name));

	/*
	 * NOTE: function has compatible semantics for str_len < 0.
	 */
	gtk_clipboard_set_text(clipboard, str, str_len);

	gdk_threads_leave();
}

gchar *
InterfaceGtk::get_clipboard(const gchar *name, gsize *str_len)
{
	GtkClipboard *clipboard;
	gchar *str;

	gdk_threads_enter();

	clipboard = gtk_clipboard_get(get_selection_by_name(name));
	/*
	 * Could return NULL for an empty clipboard.
	 * NOTE: This converts to UTF8 and we loose the ability
	 * to get clipboard with embedded nulls.
	 */
	str = gtk_clipboard_wait_for_text(clipboard);

	gdk_threads_leave();

	if (str_len)
		*str_len = str ? strlen(str) : 0;
	return str;
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

	gdk_threads_enter();

	gtk_info_popup_add(GTK_INFO_POPUP(popup_widget),
			   type2gtk[type], name, highlight);

	gdk_threads_leave();
}

void
InterfaceGtk::popup_show_impl(void)
{
	gdk_threads_enter();

	if (gtk_widget_get_visible(popup_widget))
		gtk_info_popup_scroll_page(GTK_INFO_POPUP(popup_widget));
	else
		gtk_widget_show(popup_widget);

	gdk_threads_leave();
}

void
InterfaceGtk::popup_clear_impl(void)
{
	gdk_threads_enter();

	if (gtk_widget_get_visible(popup_widget)) {
		gtk_widget_hide(popup_widget);
		gtk_info_popup_clear(GTK_INFO_POPUP(popup_widget));
	}

	gdk_threads_leave();
}

void
InterfaceGtk::set_css_variables_from_view(ViewGtk *view)
{
	guint font_size;
	gchar buffer[256];

	/*
	 * Unfortunately, we cannot use CSS variables to pass around
	 * font names and sizes, necessary for styling the command line
	 * widget.
	 * Therefore we just style it using generated CSS here.
	 * This one of the few non-deprecated ways that Gtk leaves us
	 * to set a custom font name.
	 * CSS customizations have to take that into account.
	 * NOTE: We don't actually know apriori how
	 * large our buffer should be, but luckily STYLEGETFONT with
	 * a sptr==0 will return only the size.
	 * This is undocumented in the Scintilla docs.
	 */
	gchar font_name[view->ssm(SCI_STYLEGETFONT, STYLE_DEFAULT) + 1];
	view->ssm(SCI_STYLEGETFONT, STYLE_DEFAULT, (sptr_t)font_name);
	font_size = view->ssm(SCI_STYLEGETSIZEFRACTIONAL, STYLE_DEFAULT);

	/*
	 * Generates a CSS that sets some predefined color variables.
	 * This effectively "exports" Scintilla styles into the CSS
	 * world.
	 * Those colors are used by the fallback.css shipping with SciTECO
	 * in order to apply the SciTECO-controlled color scheme to all the
	 * predefined UI elements.
	 * They can also be used in user-customizations.
	 */
	g_snprintf(buffer, sizeof(buffer),
	           "@define-color sciteco_default_fg_color " CSS_COLOR_FORMAT ";"
	           "@define-color sciteco_default_bg_color " CSS_COLOR_FORMAT ";"
	           "@define-color sciteco_calltip_fg_color " CSS_COLOR_FORMAT ";"
	           "@define-color sciteco_calltip_bg_color " CSS_COLOR_FORMAT ";"
	           "#%s{"
	           "font: %s %u.%u"
	           "}",
	           bgr2rgb(view->ssm(SCI_STYLEGETFORE, STYLE_DEFAULT)),
	           bgr2rgb(view->ssm(SCI_STYLEGETBACK, STYLE_DEFAULT)),
	           bgr2rgb(view->ssm(SCI_STYLEGETFORE, STYLE_CALLTIP)),
	           bgr2rgb(view->ssm(SCI_STYLEGETBACK, STYLE_CALLTIP)),
	           gtk_widget_get_name(cmdline_widget),
	           font_name,
	           font_size / SC_FONT_SIZE_MULTIPLIER,
	           font_size % SC_FONT_SIZE_MULTIPLIER);

	/*
	 * The GError and return value has been deprecated.
	 * A CSS parsing error would point to a programming
	 * error anyway.
	 */
	gtk_css_provider_load_from_data(css_var_provider, buffer, -1, NULL);
}

void
InterfaceGtk::event_loop_impl(void)
{
	static const gchar *icon_files[] = {
		SCITECODATADIR G_DIR_SEPARATOR_S "sciteco-16.png",
		SCITECODATADIR G_DIR_SEPARATOR_S "sciteco-32.png",
		SCITECODATADIR G_DIR_SEPARATOR_S "sciteco-48.png",
		NULL
	};

	GdkScreen *default_screen = gdk_screen_get_default();
	GtkCssProvider *user_css_provider;
	gchar *config_path, *user_css_file;

	GList *icon_list = NULL;
	GThread *thread;

	/*
	 * Assign an icon to the window.
	 * If the file could not be found, we fail silently.
	 * FIXME: On Windows, it may be better to load the icon compiled
	 * as a resource into the binary.
	 */
	for (const gchar **file = icon_files; *file; file++) {
		GdkPixbuf *icon_pixbuf = gdk_pixbuf_new_from_file(*file, NULL);

		/* fail silently if there's a problem with one of the icons */
		if (icon_pixbuf)
			icon_list = g_list_append(icon_list, icon_pixbuf);
	}

	gtk_window_set_default_icon_list(icon_list);

	if (icon_list)
		g_list_free_full(icon_list, g_object_unref);

	refresh_info();

	/*
	 * Initialize the CSS variable provider and the CSS provider
	 * for the included fallback.css.
	 * NOTE: The return value of gtk_css_provider_load() is deprecated.
	 * Instead we could register for the "parsing-error" signal.
	 * For the time being we just silently ignore parsing errors.
	 * They will be printed to stderr by Gtk anyway.
	 */
	css_var_provider = gtk_css_provider_new();
	if (current_view)
		/* set CSS variables initially */
		set_css_variables_from_view(current_view);
	gtk_style_context_add_provider_for_screen(default_screen,
	                                          GTK_STYLE_PROVIDER(css_var_provider),
	                                          GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	user_css_provider = gtk_css_provider_new();
	/* get path of $SCITECOCONFIG/.teco_css */
	config_path = QRegisters::globals["$SCITECOCONFIG"]->get_string();
	user_css_file = g_build_filename(config_path, USER_CSS_FILE, NIL);
	if (g_file_test(user_css_file, G_FILE_TEST_IS_REGULAR))
		/* open user CSS */
		gtk_css_provider_load_from_path(user_css_provider,
		                                user_css_file, NULL);
	else
		/* use fallback CSS */
		gtk_css_provider_load_from_path(user_css_provider,
		                                SCITECODATADIR G_DIR_SEPARATOR_S
		                                "fallback.css",
		                                NULL);
	g_free(user_css_file);
	g_free(config_path);
	gtk_style_context_add_provider_for_screen(default_screen,
	                                          GTK_STYLE_PROVIDER(user_css_provider),
	                                          GTK_STYLE_PROVIDER_PRIORITY_USER);

	/*
	 * When changing views, the new widget is not
	 * added immediately to avoid flickering in the GUI.
	 * It is only updated once per key press and only
	 * if it really changed.
	 * Therefore we must add the current view to the
	 * window initially.
	 * For the same reason, window title updates are
	 * deferred to once after every key press, so we must
	 * set the window title initially.
	 */
	if (current_view) {
		current_view_widget = current_view->get_widget();
		gtk_container_add(GTK_CONTAINER(event_box_widget),
		                  current_view_widget);
	}

	gtk_widget_show_all(window);
	/* don't show popup by default */
	gtk_widget_hide(popup_widget);

	/*
	 * SIGTERM emulates the "Close" key just like when
	 * closing the window if supported by this version of glib.
	 * Note that this replaces SciTECO's default SIGTERM handler
	 * so it will additionally raise(SIGINT).
	 */
#ifdef SCITECO_HANDLE_SIGNALS
	g_unix_signal_add(SIGTERM, sigterm_handler, event_queue);
#endif

	/*
	 * Start up SciTECO execution thread.
	 * Whenever it needs to send a Scintilla message
	 * it locks the GDK mutex.
	 */
	thread = g_thread_new("sciteco-exec",
	                      exec_thread_cb, event_queue);

	/*
	 * NOTE: The watchers do not modify any GTK objects
	 * using one of the methods that lock the GDK mutex.
	 * This is from now on reserved to the execution
	 * thread. Therefore there can be no dead-locks.
	 */
	gdk_threads_enter();
	gtk_main();
	gdk_threads_leave();

	/*
	 * This usually means that the user requested program
	 * termination and the execution thread called
	 * gtk_main_quit().
	 * We still wait for the execution thread to shut down
	 * properly. This also frees `thread`.
	 */
	g_thread_join(thread);

	/*
	 * Make sure the window is hidden
	 * now already, as there may be code that has to be
	 * executed in batch mode.
	 */
	gtk_widget_hide(window);
}

static gpointer
exec_thread_cb(gpointer data)
{
	GAsyncQueue *event_queue = (GAsyncQueue *)data;

	for (;;) {
		GdkEventKey *event = (GdkEventKey *)g_async_queue_pop(event_queue);

		bool is_shift = event->state & GDK_SHIFT_MASK;
		bool is_ctl   = event->state & GDK_CONTROL_MASK;

		try {
			sigint_occurred = FALSE;
			interface.handle_key_press(is_shift, is_ctl, event->keyval);
			sigint_occurred = FALSE;
		} catch (Quit) {
			/*
			 * SciTECO should terminate, so we exit
			 * this thread.
			 * The main loop will terminate and
			 * event_loop() will return.
			 */
			gdk_event_free((GdkEvent *)event);

			gdk_threads_enter();
			gtk_main_quit();
			gdk_threads_leave();
			break;
		}

		gdk_event_free((GdkEvent *)event);
	}

	return NULL;
}

void
InterfaceGtk::handle_key_press(bool is_shift, bool is_ctl, guint keyval)
{
	GdkWindow *view_window;
	ViewGtk *last_view = current_view;

	/*
	 * Avoid redraws of the current view by freezing updates
	 * on the view's GDK window (we're running in parallel
	 * to the main loop so there could be frequent redraws).
	 * By freezing updates, the behaviour is similar to
	 * the Curses UI.
	 */
	gdk_threads_enter();
	view_window = gtk_widget_get_parent_window(event_box_widget);
	gdk_window_freeze_updates(view_window);
	gdk_threads_leave();

	switch (keyval) {
	case GDK_KEY_Escape:
		cmdline.keypress(CTL_KEY_ESC);
		break;
	case GDK_KEY_BackSpace:
		cmdline.keypress(CTL_KEY('H'));
		break;
	case GDK_KEY_Tab:
		cmdline.keypress('\t');
		break;
	case GDK_KEY_Return:
		cmdline.keypress('\n');
		break;

	/*
	 * Function key macros
	 */
#define FN(KEY, MACRO) \
	case GDK_KEY_##KEY: cmdline.fnmacro(#MACRO); break
#define FNS(KEY, MACRO) \
	case GDK_KEY_##KEY: cmdline.fnmacro(is_shift ? "S" #MACRO : #MACRO); break
	FN(Down, DOWN); FN(Up, UP);
	FNS(Left, LEFT); FNS(Right, RIGHT);
	FN(KP_Down, DOWN); FN(KP_Up, UP);
	FNS(KP_Left, LEFT); FNS(KP_Right, RIGHT);
	FNS(Home, HOME);
	case GDK_KEY_F1...GDK_KEY_F35: {
		gchar macro_name[3+1];

		g_snprintf(macro_name, sizeof(macro_name),
			   "F%d", keyval - GDK_KEY_F1 + 1);
		cmdline.fnmacro(macro_name);
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
	FN(Close, CLOSE);
#undef FNS
#undef FN

	/*
	 * Control keys and keys with printable representation
	 */
	default:
		gunichar u = gdk_keyval_to_unicode(keyval);

		if (u && g_unichar_to_utf8(u, NULL) == 1) {
			gchar key;

			g_unichar_to_utf8(u, &key);
			if (key > 0x7F)
				break;
			if (is_ctl)
				key = CTL_KEY(g_ascii_toupper(key));

			cmdline.keypress(key);
		}
	}

	/*
	 * The styles configured via Scintilla might change
	 * with every keypress.
	 */
	set_css_variables_from_view(current_view);

	/*
	 * The info area is updated very often and setting the
	 * window title each time it is updated is VERY costly.
	 * So we set it here once after every keypress even if the
	 * info line did not change.
	 * View changes are also only applied here to the GTK
	 * window even though GDK updates have been frozen since
	 * the size reallocations are very costly.
	 */
	gdk_threads_enter();

	refresh_info();

	if (current_view != last_view) {
		/*
		 * The last view's object is not guaranteed to
		 * still exist.
		 * However its widget is, due to reference counting.
		 */
		if (current_view_widget)
			gtk_container_remove(GTK_CONTAINER(event_box_widget),
			                     current_view_widget);

		current_view_widget = current_view->get_widget();

		gtk_container_add(GTK_CONTAINER(event_box_widget),
		                  current_view_widget);
		gtk_widget_show(current_view_widget);
	}

	gdk_window_thaw_updates(view_window);

	gdk_threads_leave();
}

InterfaceGtk::~InterfaceGtk()
{
	g_free(info_current);

	if (window)
		gtk_widget_destroy(window);

	scintilla_release_resources();

	if (event_queue) {
		GdkEvent *e;

		while ((e = (GdkEvent *)g_async_queue_try_pop(event_queue)))
			gdk_event_free(e);

		g_async_queue_unref(event_queue);
	}

	if (css_var_provider)
		g_object_unref(css_var_provider);
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
cmdline_key_pressed_cb(GtkWidget *widget, GdkEventKey *event,
                       gpointer user_data)
{
	GAsyncQueue *event_queue = (GAsyncQueue *)user_data;

	bool is_ctl = event->state & GDK_CONTROL_MASK;

#ifdef DEBUG
	g_printf("KEY \"%s\" (%d) SHIFT=%d CNTRL=%d\n",
		 event->string, *event->string,
		 event->state & GDK_SHIFT_MASK, event->state & GDK_CONTROL_MASK);
#endif

	g_async_queue_lock(event_queue);

	if (g_async_queue_length_unlocked(event_queue) >= 0 &&
	    is_ctl && gdk_keyval_to_upper(event->keyval) == GDK_KEY_C) {
		/*
		 * Handle asynchronous interruptions if CTRL+C is pressed.
		 * This will usually send SIGINT to the entire process
		 * group and set `sigint_occurred`.
		 * If the execution thread is currently blocking,
		 * the key is delivered like an ordinary key press.
		 */
		interrupt();
	} else {
		/*
		 * Copies the key-press event, since it must be evaluated
		 * by the exec_thread_cb. This is costly, but since we're
		 * using the event queue as a kind of keyboard buffer,
		 * who cares?
		 */
		g_async_queue_push_unlocked(event_queue,
		                            gdk_event_copy((GdkEvent *)event));
	}

	g_async_queue_unlock(event_queue);

	return TRUE;
}

static gboolean
window_delete_cb(GtkWidget *w, GdkEventAny *e, gpointer user_data)
{
	GAsyncQueue *event_queue = (GAsyncQueue *)user_data;
	GdkEventKey *close_event;

	/*
	 * Emulate that the "close" key was pressed
	 * which may then be handled by the execution thread
	 * which invokes the appropriate "function key macro"
	 * if it exists. Its default action will ensure that
	 * the execution thread shuts down and the main loop
	 * will eventually terminate.
	 */
	close_event = (GdkEventKey *)gdk_event_new(GDK_KEY_PRESS);
	close_event->window = gtk_widget_get_parent_window(w);
	close_event->keyval = GDK_KEY_Close;

	g_async_queue_push(event_queue, close_event);

	return TRUE;
}

static gboolean
sigterm_handler(gpointer user_data)
{
	GAsyncQueue *event_queue = (GAsyncQueue *)user_data;
	GdkEventKey *close_event;

	/*
	 * Since this handler replaces the default one, we
	 * also have to make sure it interrupts.
	 */
	interrupt();

	/*
	 * Similar to window deletion - emulate "close" key press.
	 */
	close_event = (GdkEventKey *)gdk_event_new(GDK_KEY_PRESS);
	close_event->keyval = GDK_KEY_Close;

	g_async_queue_push(event_queue, close_event);

	return G_SOURCE_CONTINUE;
}

} /* namespace SciTECO */
