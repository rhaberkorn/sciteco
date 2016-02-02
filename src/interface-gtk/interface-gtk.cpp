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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdarg.h>
#include <string.h>
#include <signal.h>

#include <glib.h>
#include <glib/gprintf.h>

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
#include "gtk-info-popup.h"

#include <Scintilla.h>
#include <ScintillaWidget.h>

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

} /* extern "C" */

#define UNNAMED_FILE "(Unnamed)"

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
	g_object_ref_sink(G_OBJECT(sci));

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

	g_signal_connect(G_OBJECT(sci), SCINTILLA_NOTIFY,
			 G_CALLBACK(scintilla_notify), NULL);

	/*
	 * setup() calls Scintilla messages, so we must unlock
	 * here already to avoid deadlocks.
	 */
	gdk_threads_leave();

	setup();
}

void
InterfaceGtk::main_impl(int &argc, char **&argv)
{
	static const Cmdline empty_cmdline;
	GtkWidget *overlay_widget;
	GtkWidget *info_content;

	/*
	 * g_thread_init() is required prior to v2.32
	 * (we still support v2.28) but generates a warning
	 * on newer versions.
	 */
#if !GLIB_CHECK_VERSION(2,32,0)
	g_thread_init(NULL);
#endif
	gdk_threads_init();
	gtk_init(&argc, &argv);

	/*
	 * The event queue is initialized now, so we can
	 * pass it as user data to C-linkage callbacks.
	 */
	event_queue = g_async_queue_new();

	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(window), PACKAGE_NAME);
	g_signal_connect(G_OBJECT(window), "delete-event",
			 G_CALLBACK(window_delete_cb), event_queue);

	vbox = gtk_vbox_new(FALSE, 0);

	info_current = g_strdup(PACKAGE_NAME);

	/*
	 * The event box is the parent of all Scintilla views
	 * that should be displayed.
	 * This is handy when adding or removing current views,
	 * enabling and disabling GDK updates and in order to filter
	 * mouse and keyboard events going to Scintilla.
	 */
	overlay_widget = gtk_overlay_new();
	event_box_widget = gtk_event_box_new();
	gtk_event_box_set_above_child(GTK_EVENT_BOX(event_box_widget), TRUE);
	gtk_container_add(GTK_CONTAINER(overlay_widget), event_box_widget);
	gtk_box_pack_start(GTK_BOX(vbox), overlay_widget, TRUE, TRUE, 0);

	info_widget = gtk_info_bar_new();
	info_content = gtk_info_bar_get_content_area(GTK_INFO_BAR(info_widget));
	message_widget = gtk_label_new("");
	gtk_misc_set_alignment(GTK_MISC(message_widget), 0., 0.);
	gtk_container_add(GTK_CONTAINER(info_content), message_widget);
	gtk_box_pack_start(GTK_BOX(vbox), info_widget, FALSE, FALSE, 0);

	cmdline_widget = gtk_entry_new();
	gtk_entry_set_has_frame(GTK_ENTRY(cmdline_widget), FALSE);
	gtk_editable_set_editable(GTK_EDITABLE(cmdline_widget), FALSE);
	widget_set_font(cmdline_widget, "Courier");
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
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay_widget), popup_widget);
	g_signal_connect(overlay_widget, "get-child-position",
	                 G_CALLBACK(gtk_info_popup_get_position_in_overlay), NULL);

	gtk_widget_grab_focus(cmdline_widget);

	cmdline_update(&empty_cmdline);
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

	/*
	 * stdio_vmsg() leaves `ap` undefined and we are expected
	 * to do the same and behave like vprintf().
	 */
	va_copy(aq, ap);
	stdio_vmsg(type, fmt, ap);
	g_vsnprintf(buf, sizeof(buf), fmt, aq);
	va_end(aq);

	gdk_threads_enter();

	gtk_info_bar_set_message_type(GTK_INFO_BAR(info_widget),
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

	gtk_info_bar_set_message_type(GTK_INFO_BAR(info_widget),
				      GTK_MESSAGE_OTHER);
	gtk_label_set_text(GTK_LABEL(message_widget), "");

	gdk_threads_leave();
}

void
InterfaceGtk::show_view_impl(ViewGtk *view)
{
	current_view = view;
}

void
InterfaceGtk::info_update_impl(const QRegister *reg)
{
	gchar *name = String::canonicalize_ctl(reg->name);

	g_free(info_current);
	info_current = g_strconcat(PACKAGE_NAME " - <QRegister> ",
	                           name, NIL);
	g_free(name);
}

void
InterfaceGtk::info_update_impl(const Buffer *buffer)
{
	g_free(info_current);
	info_current = g_strconcat(PACKAGE_NAME " - <Buffer> ",
	                           buffer->filename ? : UNNAMED_FILE,
	                           buffer->dirty ? "*" : "", NIL);
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
InterfaceGtk::widget_set_font(GtkWidget *widget, const gchar *font_name)
{
	PangoFontDescription *font_desc;

	font_desc = pango_font_description_from_string(font_name);
	gtk_widget_modify_font(widget, font_desc);
	pango_font_description_free(font_desc);
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
	gtk_window_set_title(GTK_WINDOW(window), info_current);

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
	 * The info area is updated very often and setting the
	 * window title each time it is updated is VERY costly.
	 * So we set it here once after every keypress even if the
	 * info line did not change.
	 * View changes are also only applied here to the GTK
	 * window even though GDK updates have been frozen since
	 * the size reallocations are very costly.
	 */
	gdk_threads_enter();

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

	gtk_window_set_title(GTK_WINDOW(window), info_current);

	gdk_window_thaw_updates(view_window);

	gdk_threads_leave();
}

InterfaceGtk::~InterfaceGtk()
{
	g_free(info_current);
	if (popup_widget)
		gtk_widget_destroy(popup_widget);
	if (window)
		gtk_widget_destroy(window);

	scintilla_release_resources();

	if (event_queue) {
		GdkEvent *e;

		while ((e = (GdkEvent *)g_async_queue_try_pop(event_queue)))
			gdk_event_free(e);

		g_async_queue_unref(event_queue);
	}
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