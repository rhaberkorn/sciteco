/*
 * Copyright (C) 2012-2025 Robin Haberkorn
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

#ifdef G_OS_UNIX
#include <glib-unix.h>
#endif

#include <gdk/gdk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include <gtk/gtk.h>

#ifdef GDK_WINDOWING_X11
#include <gtk/gtkx.h>
#endif

#include <gio/gio.h>

#include <Scintilla.h>
#include <ScintillaWidget.h>

#include "gtk-info-popup.h"
#include "gtk-label.h"

#include "sciteco.h"
#include "error.h"
#include "string-utils.h"
#include "file-utils.h"
#include "cmdline.h"
#include "qreg.h"
#include "ring.h"
#include "memory.h"
#include "interface.h"

//#define DEBUG

static gboolean teco_interface_busy_timeout_cb(gpointer user_data);
static void teco_interface_event_box_realized_cb(GtkWidget *widget, gpointer user_data);
static void teco_interface_cmdline_size_allocate_cb(GtkWidget *widget,
                                                    GdkRectangle *allocation,
                                                    gpointer user_data);
static void teco_interface_cmdline_commit_cb(GtkIMContext *context, gchar *str,
                                             gpointer user_data);
static gboolean teco_interface_input_cb(GtkWidget *widget, GdkEvent *event,
                                        gpointer user_data);
static void teco_interface_popup_clicked_cb(GtkWidget *popup, gchar *str, gulong len,
                                            gpointer user_data);
static gboolean teco_interface_window_delete_cb(GtkWidget *widget, GdkEventAny *event,
                                                gpointer user_data);
static gboolean teco_interface_sigterm_handler(gpointer user_data) G_GNUC_UNUSED;

/**
 * Interval between polling for keypresses.
 * In other words, this is the maximum latency to detect CTRL+C interruptions.
 */
#define TECO_POLL_INTERVAL	100000 /* microseconds */

#define UNNAMED_FILE		"(Unnamed)"

#define USER_CSS_FILE		".teco_css"

/** printf() format for CSS RGB colors given as guint32 */
#define CSS_COLOR_FORMAT	"#%06" G_GINT32_MODIFIER "X"

/** Style used for the asterisk at the beginning of the command line */
#define STYLE_ASTERISK		16

/** Indicator number used for control characters in the command line */
#define INDIC_CONTROLCHAR	(INDIC_CONTAINER+0)
/** Indicator number used for the rubbed out part of the command line */
#define INDIC_RUBBEDOUT		(INDIC_CONTAINER+1)

/** Convert Scintilla-style BGR color triple to RGB. */
static inline guint32
teco_bgr2rgb(guint32 bgr)
{
	return GUINT32_SWAP_LE_BE(bgr) >> 8;
}

static struct {
	GtkCssProvider *css_var_provider;

	GtkWidget *window;

	enum {
		TECO_INFO_TYPE_BUFFER = 0,
		TECO_INFO_TYPE_BUFFER_DIRTY,
		TECO_INFO_TYPE_QREG
	} info_type;
	teco_string_t info_current;

	gboolean no_csd;
	gint xembed_id;

	GtkWidget *info_bar_widget;
	GtkWidget *info_image;
	GtkWidget *info_type_widget;
	GtkWidget *info_name_widget;

	GtkWidget *event_box_widget;

	GtkWidget *message_bar_widget;
	GtkWidget *message_widget;

	teco_view_t *cmdline_view;
	GtkIMContext *input_method;

	GtkWidget *popup_widget;
	gsize popup_prefix_len;

	GtkWidget *current_view_widget;

	GQueue *event_queue;
} teco_interface;

void
teco_interface_init(void)
{
	/*
	 * gtk_init() is not necessary when using gtk_get_option_group(),
	 * but this will open the default display.
	 *
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
	teco_qreg_table_insert(&teco_qreg_table_globals, teco_qreg_clipboard_new(""));
	teco_qreg_table_insert(&teco_qreg_table_globals, teco_qreg_clipboard_new("P"));
	teco_qreg_table_insert(&teco_qreg_table_globals, teco_qreg_clipboard_new("S"));
	teco_qreg_table_insert(&teco_qreg_table_globals, teco_qreg_clipboard_new("C"));

	teco_interface.event_queue = g_queue_new();

#ifdef GDK_WINDOWING_X11
	teco_interface.window = teco_interface.xembed_id ? gtk_plug_new(teco_interface.xembed_id)
	                                                 : gtk_window_new(GTK_WINDOW_TOPLEVEL);
#else
	teco_interface.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#endif

	g_signal_connect(teco_interface.window, "delete-event",
			 G_CALLBACK(teco_interface_window_delete_cb), NULL);

	g_signal_connect(teco_interface.window, "key-press-event",
			 G_CALLBACK(teco_interface_input_cb), NULL);

	GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

	/*
	 * The info bar is tried to be made the title bar of the
	 * window which also disables the default window decorations
	 * (client-side decorations) unless --no-csd was specified.
	 *
	 * NOTE: Client-side decoations could fail, leaving us with a
	 * standard title bar and the info bar with close buttons.
	 * Other window managers have undesirable side-effects.
	 */
	teco_interface.info_bar_widget = gtk_header_bar_new();
	gtk_widget_set_name(teco_interface.info_bar_widget, "sciteco-info-bar");
	teco_interface.info_name_widget = teco_gtk_label_new(NULL, 0);
	gtk_widget_set_valign(teco_interface.info_name_widget, GTK_ALIGN_CENTER);
	/* eases writing portable fallback.css that avoids CSS element names */
	gtk_style_context_add_class(gtk_widget_get_style_context(teco_interface.info_name_widget),
	                            "label");
	gtk_style_context_add_class(gtk_widget_get_style_context(teco_interface.info_name_widget),
	                            "name-label");
	gtk_label_set_selectable(GTK_LABEL(teco_interface.info_name_widget), TRUE);
	/* NOTE: Header bar does not resize for multi-line labels */
	//gtk_label_set_line_wrap(GTK_LABEL(teco_interface.info_name_widget), TRUE);
	//gtk_label_set_lines(GTK_LABEL(teco_interface.info_name_widget), 2);
	gtk_header_bar_set_custom_title(GTK_HEADER_BAR(teco_interface.info_bar_widget),
	                                teco_interface.info_name_widget);
	teco_interface.info_image = gtk_image_new();
	gtk_header_bar_pack_start(GTK_HEADER_BAR(teco_interface.info_bar_widget),
	                          teco_interface.info_image);
	teco_interface.info_type_widget = gtk_label_new(NULL);
	gtk_widget_set_valign(teco_interface.info_type_widget, GTK_ALIGN_CENTER);
	/* eases writing portable fallback.css that avoids CSS element names */
	gtk_style_context_add_class(gtk_widget_get_style_context(teco_interface.info_type_widget),
	                            "label");
	gtk_style_context_add_class(gtk_widget_get_style_context(teco_interface.info_type_widget),
	                            "type-label");
	gtk_header_bar_pack_start(GTK_HEADER_BAR(teco_interface.info_bar_widget),
	                          teco_interface.info_type_widget);
	if (teco_interface.xembed_id || teco_interface.no_csd ||
	    !g_strcmp0(g_getenv("GTK_CSD"), "0")) {
		/* fall back to adding the info bar as an ordinary widget */
		gtk_box_pack_start(GTK_BOX(vbox), teco_interface.info_bar_widget,
		                   FALSE, FALSE, 0);
	} else {
		/* use client-side decorations */
		gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(teco_interface.info_bar_widget), TRUE);
		gtk_window_set_titlebar(GTK_WINDOW(teco_interface.window),
		                        teco_interface.info_bar_widget);
	}

	/*
	 * Overlay widget will allow overlaying the Scintilla view
	 * and message widgets with the info popup.
	 * Therefore overlay_vbox (containing the view and popup)
	 * will be the main child of the overlay.
	 */
	GtkWidget *overlay_widget = gtk_overlay_new();
	GtkWidget *overlay_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

	/*
	 * The event box is the parent of all Scintilla views
	 * that should be displayed.
	 * This is handy when adding or removing current views,
	 * enabling and disabling GDK updates and in order to filter
	 * mouse and keyboard events going to Scintilla.
	 */
	teco_interface.event_box_widget = gtk_event_box_new();
	gtk_event_box_set_above_child(GTK_EVENT_BOX(teco_interface.event_box_widget), TRUE);
	gtk_box_pack_start(GTK_BOX(overlay_vbox), teco_interface.event_box_widget,
	                   TRUE, TRUE, 0);

	g_signal_connect(teco_interface.event_box_widget, "realize",
			 G_CALLBACK(teco_interface_event_box_realized_cb), NULL);

	gint events = gtk_widget_get_events(teco_interface.event_box_widget);
	events |= GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
	          GDK_SCROLL_MASK;
	gtk_widget_set_events(teco_interface.event_box_widget, events);

	g_signal_connect(teco_interface.event_box_widget, "button-press-event",
			 G_CALLBACK(teco_interface_input_cb), NULL);
	g_signal_connect(teco_interface.event_box_widget, "button-release-event",
			 G_CALLBACK(teco_interface_input_cb), NULL);
	g_signal_connect(teco_interface.event_box_widget, "scroll-event",
			 G_CALLBACK(teco_interface_input_cb), NULL);

	teco_interface.message_bar_widget = gtk_info_bar_new();
	gtk_widget_set_name(teco_interface.message_bar_widget, "sciteco-message-bar");
	GtkWidget *message_bar_content =
			gtk_info_bar_get_content_area(GTK_INFO_BAR(teco_interface.message_bar_widget));
	/* NOTE: Messages are always pre-canonicalized */
	teco_interface.message_widget = gtk_label_new(NULL);
	/* eases writing portable fallback.css that avoids CSS element names */
	gtk_style_context_add_class(gtk_widget_get_style_context(teco_interface.message_widget),
	                            "label");
	gtk_label_set_selectable(GTK_LABEL(teco_interface.message_widget), TRUE);
	gtk_label_set_line_wrap(GTK_LABEL(teco_interface.message_widget), TRUE);
	gtk_container_add(GTK_CONTAINER(message_bar_content), teco_interface.message_widget);
	gtk_box_pack_start(GTK_BOX(overlay_vbox), teco_interface.message_bar_widget,
	                   FALSE, FALSE, 0);

	gtk_container_add(GTK_CONTAINER(overlay_widget), overlay_vbox);
	gtk_box_pack_start(GTK_BOX(vbox), overlay_widget, TRUE, TRUE, 0);

	teco_interface.cmdline_view = teco_view_new();
	teco_view_setup(teco_interface.cmdline_view);
	teco_view_ssm(teco_interface.cmdline_view, SCI_SETUNDOCOLLECTION, FALSE, 0);
	teco_view_ssm(teco_interface.cmdline_view, SCI_SETVSCROLLBAR, FALSE, 0);
	teco_view_ssm(teco_interface.cmdline_view, SCI_SETMARGINTYPEN, 1, SC_MARGIN_TEXT);
	teco_view_ssm(teco_interface.cmdline_view, SCI_MARGINSETSTYLE, 0, STYLE_ASTERISK);
	teco_view_ssm(teco_interface.cmdline_view, SCI_SETMARGINWIDTHN, 1,
	              teco_view_ssm(teco_interface.cmdline_view, SCI_TEXTWIDTH, STYLE_ASTERISK, (sptr_t)"*"));
	teco_view_ssm(teco_interface.cmdline_view, SCI_MARGINSETTEXT, 0, (sptr_t)"*");
	/* only required as long as we avoid ordinary character representations */
	teco_view_ssm(teco_interface.cmdline_view, SCI_INDICSETSTYLE, INDIC_CONTROLCHAR, INDIC_ROUNDBOX);
	teco_view_ssm(teco_interface.cmdline_view, SCI_INDICSETALPHA, INDIC_CONTROLCHAR, 128);
	teco_view_ssm(teco_interface.cmdline_view, SCI_INDICSETSTYLE, INDIC_RUBBEDOUT, INDIC_STRIKE);
	/* we will forward key events, so the view should only react to text insertion */
	teco_view_ssm(teco_interface.cmdline_view, SCI_CLEARALLCMDKEYS, 0, 0);

	GtkWidget *cmdline_widget = GTK_WIDGET(teco_interface.cmdline_view);
	gtk_widget_set_name(cmdline_widget, "sciteco-cmdline");
	g_signal_connect(cmdline_widget, "size-allocate",
	                 G_CALLBACK(teco_interface_cmdline_size_allocate_cb), NULL);
	gtk_box_pack_start(GTK_BOX(vbox), cmdline_widget, FALSE, FALSE, 0);

	gtk_container_add(GTK_CONTAINER(teco_interface.window), vbox);

	teco_interface.input_method = gtk_im_context_simple_new();
	gtk_im_context_set_client_window(teco_interface.input_method,
	                                 gtk_widget_get_window(cmdline_widget));
	gtk_im_context_focus_in(teco_interface.input_method);
	gtk_im_context_set_use_preedit(teco_interface.input_method, FALSE);
	g_signal_connect(teco_interface.input_method, "commit",
	                 G_CALLBACK(teco_interface_cmdline_commit_cb), NULL);

	/*
	 * Popup widget will be shown in the bottom
	 * of the overlay widget (i.e. the Scintilla views),
	 * filling the entire width.
	 */
	teco_interface.popup_widget = teco_gtk_info_popup_new();
	gtk_widget_set_name(teco_interface.popup_widget, "sciteco-info-popup");
	g_signal_connect(teco_interface.popup_widget, "clicked",
	                 G_CALLBACK(teco_interface_popup_clicked_cb), NULL);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay_widget), teco_interface.popup_widget);
	g_signal_connect(overlay_widget, "get-child-position",
	                 G_CALLBACK(teco_gtk_info_popup_get_position_in_overlay), NULL);

	/*
	 * FIXME: Nothing can really take the focus, so it will end up in the
	 * selectable labels unless we explicitly prevent it.
	 */
	gtk_widget_set_can_focus(teco_interface.message_widget, FALSE);
	gtk_widget_set_can_focus(teco_interface.info_name_widget, FALSE);

	teco_cmdline_t empty_cmdline;
	memset(&empty_cmdline, 0, sizeof(empty_cmdline));
	teco_interface_cmdline_update(&empty_cmdline);
}

static void
teco_interface_set_cursor(GtkWidget *widget, const gchar *name)
{
	GdkWindow *window = gtk_widget_get_window(widget);
	g_assert(window != NULL);
	GdkDisplay *display = gdk_window_get_display(window);

	g_autoptr(GdkCursor) cursor = name ? gdk_cursor_new_from_name(display, name) : NULL;
	gdk_window_set_cursor(window, cursor);
}

GOptionGroup *
teco_interface_get_options(void)
{
	static const GOptionEntry entries[] = {
		{"no-csd", 0, G_OPTION_FLAG_IN_MAIN,
		 G_OPTION_ARG_NONE, &teco_interface.no_csd,
		 "Disable client-side decorations.", NULL},
#ifdef GDK_WINDOWING_X11
		{"xembed", 0, G_OPTION_FLAG_IN_MAIN,
		 G_OPTION_ARG_INT, &teco_interface.xembed_id,
		 "Embed into an existing X11 Window.", "ID"},
#endif
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

void teco_interface_init_color(guint color, guint32 rgb) {}

void
teco_interface_vmsg(teco_msg_t type, const gchar *fmt, va_list ap)
{
	/*
	 * The message types are chosen such that there is a CSS class
	 * for every one of them. GTK_MESSAGE_OTHER does not have
	 * a CSS class.
	 */
	static const GtkMessageType type2gtk[] = {
		[TECO_MSG_USER]		= GTK_MESSAGE_QUESTION,
		[TECO_MSG_INFO]		= GTK_MESSAGE_INFO,
		[TECO_MSG_WARNING]	= GTK_MESSAGE_WARNING,
		[TECO_MSG_ERROR]	= GTK_MESSAGE_ERROR
	};

	g_assert(type < G_N_ELEMENTS(type2gtk));

	gchar buf[256];

	/*
	 * stdio_vmsg() leaves `ap` undefined and we are expected
	 * to do the same and behave like vprintf().
	 */
	va_list aq;
	va_copy(aq, ap);
	teco_interface_stdio_vmsg(type, fmt, ap);
	g_vsnprintf(buf, sizeof(buf), fmt, aq);
	va_end(aq);

	gtk_info_bar_set_message_type(GTK_INFO_BAR(teco_interface.message_bar_widget),
				      type2gtk[type]);
	gtk_label_set_text(GTK_LABEL(teco_interface.message_widget), buf);

	if (type == TECO_MSG_ERROR)
		gtk_widget_error_bell(teco_interface.window);
}

void
teco_interface_msg_clear(void)
{
	gtk_info_bar_set_message_type(GTK_INFO_BAR(teco_interface.message_bar_widget),
				      GTK_MESSAGE_QUESTION);
	gtk_label_set_text(GTK_LABEL(teco_interface.message_widget), "");
}

void
teco_interface_show_view(teco_view_t *view)
{
	teco_interface_current_view = view;
}

static void
teco_interface_refresh_info(void)
{
	GtkStyleContext *style = gtk_widget_get_style_context(teco_interface.info_bar_widget);

	gtk_style_context_remove_class(style, "info-qregister");
	gtk_style_context_remove_class(style, "info-buffer");
	gtk_style_context_remove_class(style, "dirty");

	g_auto(teco_string_t) info_current_temp;
	teco_string_init(&info_current_temp,
	                 teco_interface.info_current.data, teco_interface.info_current.len);
	if (teco_interface.info_type == TECO_INFO_TYPE_BUFFER_DIRTY)
		teco_string_append_c(&info_current_temp, '*');
	teco_gtk_label_set_text(TECO_GTK_LABEL(teco_interface.info_name_widget),
	                        info_current_temp.data, info_current_temp.len);
	g_autofree gchar *info_current_canon =
			teco_string_echo(info_current_temp.data, info_current_temp.len);

	const gchar *info_type_str = PACKAGE;
	g_autoptr(GIcon) icon = NULL;

	switch (teco_interface.info_type) {
	case TECO_INFO_TYPE_QREG:
		gtk_style_context_add_class(style, "info-qregister");

		info_type_str = PACKAGE_NAME " - <QRegister> ";
		gtk_label_set_text(GTK_LABEL(teco_interface.info_type_widget), "QRegister");
		gtk_label_set_ellipsize(GTK_LABEL(teco_interface.info_name_widget),
		                        PANGO_ELLIPSIZE_START);

		/*
		 * FIXME: Perhaps we should use the SciTECO icon for Q-Registers.
		 */
		icon = g_icon_new_for_string("emblem-generic", NULL);
		break;

	case TECO_INFO_TYPE_BUFFER_DIRTY:
		gtk_style_context_add_class(style, "dirty");
		/* fall through */
	case TECO_INFO_TYPE_BUFFER:
		gtk_style_context_add_class(style, "info-buffer");

		info_type_str = PACKAGE_NAME " - <Buffer> ";
		gtk_label_set_text(GTK_LABEL(teco_interface.info_type_widget), "Buffer");
		gtk_label_set_ellipsize(GTK_LABEL(teco_interface.info_name_widget),
		                        PANGO_ELLIPSIZE_MIDDLE);

		icon = teco_gtk_info_popup_get_icon_for_path(teco_interface.info_current.data,
		                                             "text-x-generic");
		break;
	}

	g_autofree gchar *title = g_strconcat(info_type_str, info_current_canon, NULL);
	gtk_window_set_title(GTK_WINDOW(teco_interface.window), title);

	if (icon) {
		gint width, height;
		gtk_icon_size_lookup(GTK_ICON_SIZE_LARGE_TOOLBAR, &width, &height);

		gtk_image_set_from_gicon(GTK_IMAGE(teco_interface.info_image),
		                         icon, GTK_ICON_SIZE_LARGE_TOOLBAR);
		/* This is necessary so that oversized icons get scaled down. */
		gtk_image_set_pixel_size(GTK_IMAGE(teco_interface.info_image), height);
	}
}

void
teco_interface_info_update_qreg(const teco_qreg_t *reg)
{
	teco_string_clear(&teco_interface.info_current);
	teco_string_init(&teco_interface.info_current,
	                 reg->head.name.data, reg->head.name.len);
        teco_interface.info_type = TECO_INFO_TYPE_QREG;
}

void
teco_interface_info_update_buffer(const teco_buffer_t *buffer)
{
	const gchar *filename = buffer->filename ? : UNNAMED_FILE;

	teco_string_clear(&teco_interface.info_current);
	teco_string_init(&teco_interface.info_current, filename, strlen(filename));
	teco_interface.info_type = buffer->dirty ? TECO_INFO_TYPE_BUFFER_DIRTY
	                                         : TECO_INFO_TYPE_BUFFER;
}

/**
 * Insert a single character into the command line.
 *
 * @fixme
 * Control characters should be inserted verbatim since the Scintilla
 * representations of them should be preferred.
 * However, Scintilla would break the line on every CR/LF and there is
 * currently no way to prevent this.
 * Scintilla needs to be patched.
 *
 * @see teco_view_set_representations()
 * @see teco_curses_format_str()
 */
static void
teco_interface_cmdline_insert_c(gchar chr)
{
	gchar buffer[3+1] = "";

	/*
	 * NOTE: This mapping is similar to teco_view_set_representations()
	 */
	switch (chr) {
	case '\e': strcpy(buffer, "$"); break;
	case '\r': strcpy(buffer, "CR"); break;
	case '\n': strcpy(buffer, "LF"); break;
	case '\t': strcpy(buffer, "TAB"); break;
	default:
		if (TECO_IS_CTL(chr)) {
			buffer[0] = '^';
			buffer[1] = TECO_CTL_ECHO(chr);
			buffer[2] = '\0';
		}
	}

	if (*buffer) {
		gsize len = strlen(buffer);
		teco_view_ssm(teco_interface.cmdline_view, SCI_APPENDTEXT, len, (sptr_t)buffer);
		teco_view_ssm(teco_interface.cmdline_view, SCI_SETINDICATORCURRENT, INDIC_CONTROLCHAR, 0);
		teco_view_ssm(teco_interface.cmdline_view, SCI_INDICATORFILLRANGE,
		              teco_view_ssm(teco_interface.cmdline_view, SCI_GETLENGTH, 0, 0) - len, len);
	} else {
		teco_view_ssm(teco_interface.cmdline_view, SCI_APPENDTEXT, 1, (sptr_t)&chr);
	}
}

void
teco_interface_cmdline_update(const teco_cmdline_t *cmdline)
{
	/*
	 * We don't know if the new command line is similar to
	 * the old one, so we can just as well rebuild it.
	 *
	 * NOTE: teco_view_ssm() already locks the GDK lock.
	 */
	teco_view_ssm(teco_interface.cmdline_view, SCI_CLEARALL, 0, 0);

	/* format effective command line */
	for (guint i = 0; i < cmdline->effective_len; i++)
		teco_interface_cmdline_insert_c(cmdline->str.data[i]);

	/* cursor should be after effective command line */
	guint pos = teco_view_ssm(teco_interface.cmdline_view, SCI_GETLENGTH, 0, 0);
	teco_view_ssm(teco_interface.cmdline_view, SCI_GOTOPOS, pos, 0);

	/* format rubbed out command line */
	for (guint i = cmdline->effective_len; i < cmdline->str.len; i++)
		teco_interface_cmdline_insert_c(cmdline->str.data[i]);

	teco_view_ssm(teco_interface.cmdline_view, SCI_SETINDICATORCURRENT, INDIC_RUBBEDOUT, 0);
	teco_view_ssm(teco_interface.cmdline_view, SCI_INDICATORFILLRANGE, pos,
	              teco_view_ssm(teco_interface.cmdline_view, SCI_GETLENGTH, 0, 0) - pos);

	teco_view_ssm(teco_interface.cmdline_view, SCI_SCROLLCARET, 0, 0);
}

static GdkAtom
teco_interface_get_selection_by_name(const gchar *name)
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

static void
teco_interface_clipboard_provide(GtkClipboard *clipboard, GtkSelectionData *selection, guint info, gpointer userdata)
{
	GString *str = userdata;
	gint len = str->len;
#ifdef G_OS_WIN32
	/* include trailing null byte */
	len++;
#endif
	gtk_selection_data_set_text(selection, str->str, len);
}

static void
teco_interface_clipboard_clear(GtkClipboard *clipboard, gpointer userdata)
{
	GString *str = userdata;
	g_string_free(str, TRUE);
}

gboolean
teco_interface_set_clipboard(const gchar *name, const gchar *str, gsize str_len, GError **error)
{
	static const GtkTargetEntry target = {"UTF8_STRING", 0, 0};
	GtkClipboard *clipboard = gtk_clipboard_get(teco_interface_get_selection_by_name(name));

	if (!str) {
		gtk_clipboard_clear(clipboard);
		return TRUE;
	}

	/*
	 * NOTE: gtk_clipboard_set_text() would ignore embedded nulls,
	 * even though it takes a length.
	 * We could theoretically avoid one allocation, but don't yet have proper types
	 * to store string data with length in one heap object.
	 */
	GString *gstr = g_string_new_len(str, str_len);
	if (!gtk_clipboard_set_with_data(clipboard, &target, 1,
	                                 teco_interface_clipboard_provide,
	                                 teco_interface_clipboard_clear, gstr)) {
		g_string_free(gstr, TRUE);
		g_set_error_literal(error, TECO_ERROR, TECO_ERROR_CLIPBOARD,
		                    "Cannot set clipboard");
		return FALSE;
	}

	return TRUE;
}

gboolean
teco_interface_get_clipboard(const gchar *name, gchar **str, gsize *len, GError **error)
{
	GtkClipboard *clipboard = gtk_clipboard_get(teco_interface_get_selection_by_name(name));
	/*
	 * gtk_clipboard_wait_for_text() does not return the text length,
	 * so it doesn't work with embedded nulls.
	 * gtk_clipboard_wait_for_contents() could also return NULL for empty clipboards.
	 *
	 * NOTE: This also drives the main event loop,
	 * which should be safe (see teco_interface_key_pressed_cb()).
	 */
	GdkAtom utf8_string = gdk_atom_intern_static_string("UTF8_STRING");
	g_autoptr(GtkSelectionData) contents = gtk_clipboard_wait_for_contents(clipboard, utf8_string);
	if (!contents) {
		*len = 0;
		if (str)
			*str = NULL;
		return TRUE;
	}

	*len = gtk_selection_data_get_length(contents);
#ifdef G_OS_WIN32
	/* the length always contains a trailing null byte on Windows */
	if (*len > 0)
		(*len)--;
#endif
	if (str) {
		/* gtk_selection_data_get_text() does not work with embedded nulls */
		*str = memcpy(g_malloc(*len+1), gtk_selection_data_get_data(contents), *len);
		(*str)[*len] = '\0';
	}

	return TRUE;
}

void
teco_interface_popup_add(teco_popup_entry_type_t type, const gchar *name, gsize name_len,
                         gboolean highlight)
{
	teco_gtk_info_popup_add(TECO_GTK_INFO_POPUP(teco_interface.popup_widget),
	                        type, name, name_len, highlight);
}

void
teco_interface_popup_show(gsize prefix_len)
{
	teco_interface.popup_prefix_len = prefix_len;
	gtk_widget_show(teco_interface.popup_widget);
}

void
teco_interface_popup_scroll(void)
{
	teco_gtk_info_popup_scroll_page(TECO_GTK_INFO_POPUP(teco_interface.popup_widget));
}

gboolean
teco_interface_popup_is_shown(void)
{
	return gtk_widget_get_visible(teco_interface.popup_widget);
}

void
teco_interface_popup_clear(void)
{
	if (gtk_widget_get_visible(teco_interface.popup_widget)) {
		gtk_widget_hide(teco_interface.popup_widget);
		teco_gtk_info_popup_clear(TECO_GTK_INFO_POPUP(teco_interface.popup_widget));
	}
}

/**
 * Whether the execution has been interrupted (CTRL+C).
 *
 * This is called regularily, so it is used to drive the
 * main loop so that we can still process key presses.
 *
 * This approach is significantly slower in interactive mode
 * than executing in a separate thread probably due to the
 * system call overhead.
 * But the GDK lock that would be necessary for synchronization
 * has been deprecated.
 */
gboolean
teco_interface_is_interrupted(void)
{
	if (!gtk_main_level())
		/* batch mode */
		return teco_interrupted != FALSE;

	/*
	 * By polling only every TECO_POLL_INTERVAL microseconds
	 * we save 75-90% of runtime.
	 */
	static guint64 last_poll_ts = 0;
	guint64 now_ts = g_get_monotonic_time();

	if (G_LIKELY(last_poll_ts+TECO_POLL_INTERVAL > now_ts))
		return teco_interrupted != FALSE;
	last_poll_ts = now_ts;

	gtk_main_iteration_do(FALSE);
	return teco_interrupted != FALSE;
}

static void
teco_interface_set_css_variables(teco_view_t *view)
{
	guint32 default_fg_color = teco_view_ssm(view, SCI_STYLEGETFORE, STYLE_DEFAULT, 0);
	guint32 default_bg_color = teco_view_ssm(view, SCI_STYLEGETBACK, STYLE_DEFAULT, 0);
	guint32 calltip_fg_color = teco_view_ssm(view, SCI_STYLEGETFORE, STYLE_CALLTIP, 0);
	guint32 calltip_bg_color = teco_view_ssm(view, SCI_STYLEGETBACK, STYLE_CALLTIP, 0);

	/*
	 * FIXME: Font and colors of Scintilla views cannot be set via CSS.
	 * But some day, there will be a way to send messages to the commandline view
	 * from SciTECO code via ES.
	 * Configuration will then be in the hands of color schemes.
	 *
	 * NOTE: We don't actually know apriori how large the font_size buffer should be,
	 * but luckily SCI_STYLEGETFONT with a sptr==0 will return only the size.
	 * This is undocumented in the Scintilla docs.
	 */
	g_autofree gchar *font_name = g_malloc(teco_view_ssm(view, SCI_STYLEGETFONT, STYLE_DEFAULT, 0) + 1);
	teco_view_ssm(view, SCI_STYLEGETFONT, STYLE_DEFAULT, (sptr_t)font_name);

	teco_view_ssm(teco_interface.cmdline_view, SCI_STYLESETFORE, STYLE_DEFAULT, default_fg_color);
	teco_view_ssm(teco_interface.cmdline_view, SCI_STYLESETBACK, STYLE_DEFAULT, default_bg_color);
	teco_view_ssm(teco_interface.cmdline_view, SCI_STYLESETFONT, STYLE_DEFAULT, (sptr_t)font_name);
	teco_view_ssm(teco_interface.cmdline_view, SCI_STYLESETSIZE, STYLE_DEFAULT,
	              teco_view_ssm(view, SCI_STYLEGETSIZE, STYLE_DEFAULT, 0));
	teco_view_ssm(teco_interface.cmdline_view, SCI_STYLECLEARALL, 0, 0);
	teco_view_ssm(teco_interface.cmdline_view, SCI_STYLESETFORE, STYLE_CALLTIP, calltip_fg_color);
	teco_view_ssm(teco_interface.cmdline_view, SCI_STYLESETBACK, STYLE_CALLTIP, calltip_bg_color);
	teco_view_ssm(teco_interface.cmdline_view, SCI_SETCARETFORE,
	              teco_view_ssm(view, SCI_GETCARETFORE, 0, 0), 0);
	/* used for the asterisk at the beginning of the command line */
	teco_view_ssm(teco_interface.cmdline_view, SCI_STYLESETBOLD, STYLE_ASTERISK, TRUE);
	/* used for character representations */
	teco_view_ssm(teco_interface.cmdline_view, SCI_INDICSETFORE, INDIC_CONTROLCHAR, default_fg_color);
	/* used for the rubbed out command line */
	teco_view_ssm(teco_interface.cmdline_view, SCI_INDICSETFORE, INDIC_RUBBEDOUT, default_fg_color);
	/* this somehow gets reset */
	teco_view_ssm(teco_interface.cmdline_view, SCI_MARGINSETTEXT, 0, (sptr_t)"*");

	guint text_height = teco_view_ssm(teco_interface.cmdline_view, SCI_TEXTHEIGHT, 0, 0);

	/*
	 * Generates a CSS that sets some predefined color variables.
	 * This effectively "exports" Scintilla styles into the CSS
	 * world.
	 * Those colors are used by the fallback.css shipping with SciTECO
	 * in order to apply the SciTECO-controlled color scheme to all the
	 * predefined UI elements.
	 * They can also be used in user-customizations.
	 */
	gchar css[256];
	g_snprintf(css, sizeof(css),
	           "@define-color sciteco_default_fg_color " CSS_COLOR_FORMAT ";"
	           "@define-color sciteco_default_bg_color " CSS_COLOR_FORMAT ";"
	           "@define-color sciteco_calltip_fg_color " CSS_COLOR_FORMAT ";"
	           "@define-color sciteco_calltip_bg_color " CSS_COLOR_FORMAT ";",
	           teco_bgr2rgb(default_fg_color), teco_bgr2rgb(default_bg_color),
	           teco_bgr2rgb(calltip_fg_color), teco_bgr2rgb(calltip_bg_color));

	/*
	 * The GError and return value has been deprecated.
	 * A CSS parsing error would point to a programming
	 * error anyway.
	 */
	gtk_css_provider_load_from_data(teco_interface.css_var_provider, css, -1, NULL);

	/*
	 * The font and size of the commandline view might have changed,
	 * so we resize it.
	 * This cannot be done via CSS or Scintilla messages.
	 * Currently, it is always exactly one line high in order to mimic the Curses UI.
	 */
	gtk_widget_set_size_request(GTK_WIDGET(teco_interface.cmdline_view), -1, text_height);
}

static void
teco_interface_refresh(gboolean current_view_changed)
{
	/*
	 * The styles configured via Scintilla might change
	 * with every keypress.
	 */
	if (teco_interface_current_view)
		teco_interface_set_css_variables(teco_interface_current_view);

	/*
	 * The info area is updated very often and setting the
	 * window title each time it is updated is VERY costly.
	 * So we set it here once after every keypress even if the
	 * info line did not change.
	 * View changes are also only applied here to the GTK
	 * window even though GDK updates have been frozen since
	 * the size reallocations are very costly.
	 */
	teco_interface_refresh_info();

	if (current_view_changed) {
		/*
		 * The last view's object is not guaranteed to
		 * still exist.
		 * However its widget is, due to reference counting.
		 */
		if (teco_interface.current_view_widget)
			gtk_container_remove(GTK_CONTAINER(teco_interface.event_box_widget),
			                     teco_interface.current_view_widget);

		teco_interface.current_view_widget = GTK_WIDGET(teco_interface_current_view);

		/*
		 * NOTE: The hiding helps to ensure the correct GdkWindow
		 * ordering, which is important at least for setting the
		 * mouse cursors.
		 */
		gtk_widget_hide(teco_interface.event_box_widget);
		gtk_container_add(GTK_CONTAINER(teco_interface.event_box_widget),
		                  teco_interface.current_view_widget);
		gtk_widget_show_all(teco_interface.event_box_widget);
	}
}

static void
teco_interface_cmdline_commit_cb(GtkIMContext *context, gchar *str, gpointer user_data)
{
	g_autoptr(GError) error = NULL;

	if (!teco_cmdline_keypress(str, strlen(str), &error) &&
	    g_error_matches(error, TECO_ERROR, TECO_ERROR_QUIT))
		gtk_main_quit();
}

/**
 * Try to find an ANSI (latin) key for a given keypress.
 *
 * If the given key press does not generate a key from the ANSI
 * range, it tries to find one in another group.
 *
 * @param event Key event to look up. In case of success,
 *   this event structure might also be written to.
 * @return The codepoint of the ANSI version or 0 if there is
 *   no fitting ANSI/latin key.
 */
static gchar
teco_interface_get_ansi_key(GdkEventKey *event)
{
	gunichar cp = gdk_keyval_to_unicode(event->keyval);
	if (cp && cp < 0x80)
		return cp;

	const gchar *name = gdk_keyval_name(event->keyval);
	if (name && g_str_has_prefix(name, "dead_"))
		/* we won't find the non-dead keyval anyway */
		return 0;

	GdkKeymap *map = gdk_keymap_get_for_display(gdk_window_get_display(event->window));
	g_autofree GdkKeymapKey *keys = NULL;
	g_autofree guint *keyvals = NULL;
	gint n_entries = 0;

	gdk_keymap_get_entries_for_keycode(map, event->hardware_keycode,
	                                   &keys, &keyvals, &n_entries);
	for (gint i = 0; i < n_entries; i++) {
		g_assert(keys[i].keycode == event->hardware_keycode);
		cp = gdk_keyval_to_unicode(keyvals[i]);
		if (cp && cp < 0x80 &&
		    gdk_keyval_is_upper(keyvals[i]) == gdk_keyval_is_upper(event->keyval)) {
			event->keyval = keyvals[i];
			event->group = keys[i].group;
			return cp;
		}
	}

	return 0;
}

static gboolean
teco_interface_handle_key_press(GdkEventKey *event, GError **error)
{
	g_assert(event->type == GDK_KEY_PRESS);

	switch (event->keyval) {
	case GDK_KEY_Escape:
		if (!teco_cmdline_keymacro_c('\e', error))
			return FALSE;
		break;
	case GDK_KEY_BackSpace:
		if (!teco_cmdline_keymacro_c(TECO_CTL_KEY('H'), error))
			return FALSE;
		break;
	case GDK_KEY_Tab:
		if (!teco_cmdline_keymacro_c('\t', error))
			return FALSE;
		break;
	case GDK_KEY_Return:
		if (!teco_cmdline_keymacro_c('\n', error))
			return FALSE;
		break;

	/*
	 * Function key macros
	 */
#define FN(KEY, MACRO) \
	case GDK_KEY_##KEY: \
		if (!teco_cmdline_keymacro(#MACRO, -1, error)) \
			return FALSE; \
		break
#define FNS(KEY, MACRO) \
	case GDK_KEY_##KEY: \
		if (!teco_cmdline_keymacro(event->state & GDK_SHIFT_MASK ? "S" #MACRO : #MACRO, -1, error)) \
			return FALSE; \
		break
	FN(Down, DOWN); FN(Up, UP);
	FNS(Left, LEFT); FNS(Right, RIGHT);
	FN(KP_Down, DOWN); FN(KP_Up, UP);
	FNS(KP_Left, LEFT); FNS(KP_Right, RIGHT);
	FNS(Home, HOME);
	case GDK_KEY_F1...GDK_KEY_F35: {
		gchar macro_name[3+1];

		g_snprintf(macro_name, sizeof(macro_name),
		           "F%d", event->keyval - GDK_KEY_F1 + 1);
		if (!teco_cmdline_keymacro(macro_name, -1, error))
			return FALSE;
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
		/*
		 * NOTE: Alt-Gr key-combinations are sometimes reported as
		 * Ctrl+Alt, so we filter those out.
		 */
		if ((event->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK)) == GDK_CONTROL_MASK) {
			gchar c = teco_interface_get_ansi_key(event);
			if (c) {
				if (!teco_cmdline_keymacro_c(TECO_CTL_KEY(g_ascii_toupper(c)), error))
					return FALSE;
				break;
			}
		}

		/*
		 * First look up a key macro.
		 * Only if it's undefined, we try to automatically find an ANSI key.
		 * On the downside, this means we cannot define key macros for dead keys
		 * or keys that require some sort of input method editing.
		 *
		 * FIXME: This might be a good reason to be able to disable the
		 * automatic ANSIfication, as we could look up the key macro in
		 * teco_interface_cmdline_commit_cb().
		 */
		gunichar cp = gdk_keyval_to_unicode(event->keyval);
		if (cp) {
			char buf[6];
			gsize len = g_unichar_to_utf8(cp, buf);
			teco_keymacro_status_t rc = teco_cmdline_keymacro(buf, len, error);
			if (rc == TECO_KEYMACRO_ERROR)
				return FALSE;
			if (rc == TECO_KEYMACRO_SUCCESS)
				break;
			g_assert(rc == TECO_KEYMACRO_UNDEFINED);
		}

		/*
		 * If the current state is case-insensitive, it is a command name -
		 * which consists only of ANSI letters - we try to
		 * accept non-ANSI letters as well.
		 * This means, you don't have to change keyboard layouts
		 * so often.
		 * FIXME: This could be made to work with string-building constructs
		 * within Q-Register specs as well.
		 * Unfortunately, Q-Reg specs and string building can be nested
		 * indefinitely.
		 * This would effectively require a new keymacro_mask_cb().
		 */
		if ((teco_cmdline.machine.parent.current->keymacro_mask |
		     teco_cmdline.machine.expectstring.machine.parent.current->keymacro_mask) &
								TECO_KEYMACRO_MASK_CASEINSENSITIVE)
			teco_interface_get_ansi_key(event);

		/*
		 * This is necessary to handle dead keys and in the future
		 * for inputting Asian languages.
		 *
		 * FIXME: We do not yet support preediting.
		 * It would be easier to forward the event to the Scintilla
		 * widget and use its existing IM support.
		 * But this breaks the event freezing and results in flickering.
		 */
		gtk_im_context_filter_keypress(teco_interface.input_method, event);
	}

	return TRUE;
}

static gboolean
teco_interface_handle_mouse_button(GdkEventButton *event, GError **error)
{
	switch (event->type) {
	case GDK_BUTTON_PRESS:
		teco_mouse.type = TECO_MOUSE_PRESSED;
		break;
	case GDK_BUTTON_RELEASE:
		teco_mouse.type = TECO_MOUSE_RELEASED;
		break;
	case GDK_2BUTTON_PRESS:
	case GDK_3BUTTON_PRESS:
	default:
		/* delivered in addition to GDK_BUTTON_PRESS */
		return TRUE;
	}

	teco_mouse.x = event->x;
	teco_mouse.y = event->y;
	teco_mouse.button = event->button;

	teco_mouse.mods = 0;
	if (event->state & GDK_SHIFT_MASK)
		teco_mouse.mods |= TECO_MOUSE_SHIFT;
	if (event->state & GDK_CONTROL_MASK)
		teco_mouse.mods |= TECO_MOUSE_CTRL;
	/*
	 * NOTE: GTK returns MOD1 *without* SHIFT for ALT.
	 */
	if ((event->state & (GDK_MOD1_MASK | GDK_SHIFT_MASK)) == GDK_MOD1_MASK)
		teco_mouse.mods |= TECO_MOUSE_ALT;

	return teco_cmdline_keymacro("MOUSE", -1, error);
}

static gboolean
teco_interface_handle_scroll(GdkEventScroll *event, GError **error)
{
	g_assert(event->type == GDK_SCROLL);

	/*
	 * FIXME: Do we have to support GDK_SCROLL_SMOOTH?
	 */
	switch (event->direction) {
	case GDK_SCROLL_UP:
		teco_mouse.type = TECO_MOUSE_SCROLLUP;
		break;
	case GDK_SCROLL_DOWN:
		teco_mouse.type = TECO_MOUSE_SCROLLDOWN;
		break;
	default:
		return TRUE;
	}

	teco_mouse.x = event->x;
	teco_mouse.y = event->y;
	teco_mouse.button = -1;

	teco_mouse.mods = 0;
	if (event->state & GDK_SHIFT_MASK)
		teco_mouse.mods |= TECO_MOUSE_SHIFT;
	if (event->state & GDK_CONTROL_MASK)
		teco_mouse.mods |= TECO_MOUSE_CTRL;
	/*
	 * NOTE: GTK returns MOD1 *without* SHIFT for ALT.
	 */
	if ((event->state & (GDK_MOD1_MASK | GDK_SHIFT_MASK)) == GDK_MOD1_MASK)
		teco_mouse.mods |= TECO_MOUSE_ALT;

	return teco_cmdline_keymacro("MOUSE", -1, error);
}

gboolean
teco_interface_event_loop(GError **error)
{
	teco_qreg_t *scitecoconfig_reg = teco_qreg_table_find(&teco_qreg_table_globals, "$SCITECOCONFIG", 14);
	g_assert(scitecoconfig_reg != NULL);
	g_auto(teco_string_t) scitecoconfig = {NULL, 0};
	if (!scitecoconfig_reg->vtable->get_string(scitecoconfig_reg,
	                                           &scitecoconfig.data, &scitecoconfig.len, NULL, error))
		return FALSE;
	if (teco_string_contains(&scitecoconfig, '\0')) {
		g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
		                    "Null-character not allowed in filenames");
		return FALSE;
	}
	g_assert(scitecoconfig.data != NULL);

	g_autofree gchar *datadir = teco_file_get_datadir();

	/*
	 * Initialize the CSS variable provider and the CSS provider
	 * for the included fallback.css.
	 */
	teco_interface.css_var_provider = gtk_css_provider_new();
	GdkScreen *default_screen = gdk_screen_get_default();
	gtk_style_context_add_provider_for_screen(default_screen,
	                                          GTK_STYLE_PROVIDER(teco_interface.css_var_provider),
	                                          GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

	g_autofree gchar *user_css_file = g_build_filename(scitecoconfig.data, USER_CSS_FILE, NULL);
	if (!g_file_test(user_css_file, G_FILE_TEST_IS_REGULAR)) {
		/* use fallback CSS */
		g_free(user_css_file);
		user_css_file = g_build_filename(datadir, "fallback.css", NULL);
	}

	GtkCssProvider *user_css_provider = gtk_css_provider_new();
	/*
	 * NOTE: The return value of gtk_css_provider_load() is deprecated.
	 * Instead we could register for the "parsing-error" signal.
	 * For the time being we just silently ignore parsing errors.
	 * They will be printed to stderr by Gtk anyway.
	 */
	gtk_css_provider_load_from_path(user_css_provider, user_css_file, NULL);
	gtk_style_context_add_provider_for_screen(default_screen,
	                                          GTK_STYLE_PROVIDER(user_css_provider),
	                                          GTK_STYLE_PROVIDER_PRIORITY_USER);

	teco_interface_refresh(TRUE);

	gtk_widget_show_all(teco_interface.window);
	/* don't show popup by default */
	gtk_widget_hide(teco_interface.popup_widget);

#ifdef G_OS_WIN32
	/*
	 * FIXME: This is necessary so that the icon themes are found in the same
	 * directory as sciteco.exe.
	 */
	g_autofree gchar *program_path = teco_file_get_program_path();
	g_autofree gchar *theme_path = g_build_filename(program_path, "icons", NULL);
	gtk_icon_theme_prepend_search_path(gtk_icon_theme_get_default(), theme_path);
#else
	/*
	 * Load icons for the GTK window.
	 * This is not necessary on Windows since the icon included
	 * as a resource will be used by default.
	 */
	static const gchar *icon_files[] = {
		"sciteco-48.png", "sciteco-32.png", "sciteco-16.png"
	};
	GList *icon_list = NULL;

	for (gint i = 0; i < G_N_ELEMENTS(icon_files); i++) {
		g_autofree gchar *icon_file = g_build_filename(datadir, icon_files[i], NULL);
		GdkPixbuf *icon_pixbuf = gdk_pixbuf_new_from_file(icon_file, NULL);

		/* fail silently if there's a problem with one of the icons */
		if (G_LIKELY(icon_pixbuf != NULL))
			icon_list = g_list_append(icon_list, icon_pixbuf);
	}

	/*
	 * The position of this call after gtk_widget_show() is important, so that
	 * tabbed and other Xembed hosts can pick up the icon.
	 * They also do not pick up the icon if set via gtk_window_set_default_icon_list().
	 */
	gtk_window_set_icon_list(GTK_WINDOW(teco_interface.window), icon_list);

	g_list_free_full(icon_list, g_object_unref);
#endif

	/*
	 * SIGTERM emulates the "Close" key just like when
	 * closing the window if supported by this version of glib.
	 * Note that this replaces SciTECO's default SIGTERM handler
	 * so it will additionally raise(SIGINT).
	 */
#ifdef G_OS_UNIX
	g_unix_signal_add(SIGTERM, teco_interface_sigterm_handler, NULL);
#endif

	/* don't limit while waiting for input as this might be a busy operation */
	teco_memory_stop_limiting();

	gtk_main();

	/*
	 * Make sure the window is hidden
	 * now already, as there may be code that has to be
	 * executed in batch mode.
	 */
	gtk_widget_hide(teco_interface.window);

	return TRUE;
}

void
teco_interface_cleanup(void)
{
	teco_string_clear(&teco_interface.info_current);

	if (teco_interface.input_method)
		g_object_unref(teco_interface.input_method);

	if (teco_interface.window)
		gtk_widget_destroy(teco_interface.window);

	scintilla_release_resources();

	if (teco_interface.event_queue)
		g_queue_free_full(teco_interface.event_queue,
		                  (GDestroyNotify)gdk_event_free);

	if (teco_interface.css_var_provider)
		g_object_unref(teco_interface.css_var_provider);
}

/*
 * GTK+ callbacks
 */

/**
 * Called some time after processing an input event in order to show
 * business.
 *
 * The delay avoids cursor flickering during normal typing.
 *
 * @fixme It would be nicer to set the cursor for the entire window,
 * but that would apparently require another GtkEventBox, spanning everything.
 */
static gboolean
teco_interface_busy_timeout_cb(gpointer user_data)
{
	teco_interface_set_cursor(teco_interface.event_box_widget, "wait");
	return G_SOURCE_REMOVE;
}

static void
teco_interface_event_box_realized_cb(GtkWidget *widget, gpointer user_data)
{
	/* It's only now safe to get the GdkWindow. */
	teco_interface_set_cursor(widget, "text");
}

/**
 * Called when the commandline widget is resized.
 * This should ensure that the caret jumps to the middle of the command line,
 * imitating the behaviour of the current Curses command line.
 */
static void
teco_interface_cmdline_size_allocate_cb(GtkWidget *widget,
                                        GdkRectangle *allocation, gpointer user_data)
{
	teco_view_ssm(teco_interface.cmdline_view, SCI_SETXCARETPOLICY,
	              CARET_SLOP | CARET_EVEN, allocation->width/2);
}

static gboolean
teco_interface_input_cb(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
	static gboolean recursed = FALSE;

#ifdef DEBUG
	if (event->type == GDK_KEY_PRESS)
		g_printf("KEY \"%s\" (%d) SHIFT=%d CNTRL=%d\n",
			 event->key.string, *event->key.string,
			 event->key.state & GDK_SHIFT_MASK, event->key.state & GDK_CONTROL_MASK);
#endif

	if (recursed) {
		/*
		 * We're already executing, so this event is processed
		 * from gtk_main_iteration_do().
		 * Unfortunately, gtk_main_level() is still 1 in this case.
		 *
		 * We might also completely replace the watchers
		 * during execution, but the current implementation is
		 * probably easier.
		 */
		if (event->type == GDK_KEY_PRESS &&
		    event->key.state & GDK_CONTROL_MASK &&
		    gdk_keyval_to_upper(event->key.keyval) == GDK_KEY_C)
			/*
			 * Handle asynchronous interruptions if CTRL+C is pressed.
			 * If the execution thread is currently blocking,
			 * the key is delivered like an ordinary key press.
			 */
			teco_interrupted = TRUE;
		else
			g_queue_push_tail(teco_interface.event_queue,
			                  gdk_event_copy(event));

		return TRUE;
	}

	recursed = TRUE;

	GSource *busy_timeout = g_timeout_source_new(500); /* ms */
	g_source_set_callback(busy_timeout, teco_interface_busy_timeout_cb, NULL, NULL);
	g_source_attach(busy_timeout, NULL);

	teco_memory_start_limiting();

	g_queue_push_tail(teco_interface.event_queue, gdk_event_copy(event));

	GdkWindow *top_window = gdk_window_get_toplevel(gtk_widget_get_window(teco_interface.window));

	do {
		g_autoptr(GError) error = NULL;

		/*
		 * The event queue might be filled when pressing keys when SciTECO
		 * is busy executing code.
		 */
		g_autoptr(GdkEvent) event = g_queue_pop_head(teco_interface.event_queue);

		/*
		 * Avoid redraws of the current view by freezing updates
		 * on the view's GDK window (we're running in parallel
		 * to the main loop so there could be frequent redraws).
		 * By freezing updates, the behaviour is similar to
		 * the Curses UI.
		 */
		gdk_window_freeze_updates(top_window);

		const teco_view_t *last_view = teco_interface_current_view;
		sptr_t last_vpos = teco_interface_ssm(SCI_GETFIRSTVISIBLELINE, 0, 0);

		teco_interrupted = FALSE;
		switch (event->type) {
		case GDK_KEY_PRESS:
			teco_interface_handle_key_press(&event->key, &error);
			break;
		case GDK_BUTTON_PRESS:
		case GDK_2BUTTON_PRESS:
		case GDK_3BUTTON_PRESS:
		case GDK_BUTTON_RELEASE:
			teco_interface_handle_mouse_button(&event->button, &error);
			break;
		case GDK_SCROLL:
			teco_interface_handle_scroll(&event->scroll, &error);
			break;
		default:
			g_assert_not_reached();
		}
		teco_interrupted = FALSE;

		teco_interface_refresh(teco_interface_current_view != last_view);
		/* always expand folds, even after mouse clicks */
		teco_interface_unfold();
		/*
		 * Scintilla has been patched to avoid any automatic scrolling since that
		 * has been benchmarked to be a very costly operation.
		 * Instead we do it only once after almost every keypress.
		 * If possible, the vertical scrolling position is preserved, which helps
		 * for instance if the buffer contents are deleted and restored later on.
		 *
		 * The only exception is mouse events, so you can scroll the view manually
		 * in the ^KMOUSE macro, allowing dot to be outside of the view.
		 */
		if (event->type == GDK_KEY_PRESS) {
			if (teco_interface_current_view == last_view)
				teco_interface_ssm(SCI_SETFIRSTVISIBLELINE, last_vpos, 0);
			teco_interface_ssm(SCI_SCROLLCARET, 0, 0);
		}

		gdk_window_thaw_updates(top_window);

		if (g_error_matches(error, TECO_ERROR, TECO_ERROR_QUIT)) {
			gtk_main_quit();
			break;
		}

		/*
		 * This should give the UI a chance to update after every keypress.
		 * Would also be possible but tricky to implement with an idle watcher.
		 */
		while (gtk_events_pending())
			gtk_main_iteration_do(FALSE);
	} while (!g_queue_is_empty(teco_interface.event_queue));

	teco_memory_stop_limiting();

	g_source_destroy(busy_timeout);
	g_source_unref(busy_timeout);
	teco_interface_set_cursor(teco_interface.event_box_widget, "text");

	recursed = FALSE;
	return TRUE;
}

static void
teco_interface_popup_clicked_cb(GtkWidget *popup, gchar *str, gulong len, gpointer user_data)
{
	g_assert(len >= teco_interface.popup_prefix_len);
	const teco_string_t insert = {str+teco_interface.popup_prefix_len, len-teco_interface.popup_prefix_len};
	teco_machine_t *machine = &teco_cmdline.machine.parent;

	const teco_view_t *last_view = teco_interface_current_view;

	/*
	 * NOTE: It shouldn't really be necessary to catch TECO_ERROR_QUIT here.
	 * A auto completion should never result in program termination.
	 */
	if (machine->current->insert_completion_cb &&
	    !machine->current->insert_completion_cb(machine, &insert, NULL))
		return;
	teco_interface_popup_clear();
	teco_interface_cmdline_update(&teco_cmdline);

	teco_interface_refresh(teco_interface_current_view != last_view);
}

static gboolean
teco_interface_window_delete_cb(GtkWidget *widget, GdkEventAny *event, gpointer user_data)
{
	/*
	 * Emulate that the "close" key was pressed
	 * which may then be handled by the execution thread
	 * which invokes the appropriate "function key macro"
	 * if it exists. Its default action will ensure that
	 * the execution thread shuts down and the main loop
	 * will eventually terminate.
	 */
	g_autoptr(GdkEvent) close_event = gdk_event_new(GDK_KEY_PRESS);
	close_event->key.window = gtk_widget_get_parent_window(widget);
	close_event->key.keyval = GDK_KEY_Close;

	return teco_interface_input_cb(widget, close_event, NULL);
}

static gboolean
teco_interface_sigterm_handler(gpointer user_data)
{
	/*
	 * Similar to window deletion - emulate "close" key press.
	 */
	g_autoptr(GdkEvent) close_event = gdk_event_new(GDK_KEY_PRESS);
	close_event->key.keyval = GDK_KEY_Close;

	return teco_interface_input_cb(teco_interface.window, close_event, NULL);
}
