#include <stdarg.h>

#include <glib.h>
#include <glib/gprintf.h>

#include <gdk/gdk.h>
#include <gdk/gdkkeysyms.h>

#include <gtk/gtk.h>
#include "gtk-info-popup.h"

#include <Scintilla.h>
#include <SciLexer.h>
#include <ScintillaWidget.h>

#include "sciteco.h"
#include "parser.h"
#include "qbuffers.h"
#include "undo.h"

static GtkWidget *editor_widget;
static GtkWidget *cmdline_widget;
static GtkWidget *info_widget, *message_widget;

GtkInfoPopup *filename_popup;

void
cmdline_display(const gchar *cmdline_str)
{
	gtk_entry_set_text(GTK_ENTRY(cmdline_widget), cmdline_str);
	gtk_editable_set_position(GTK_EDITABLE(cmdline_widget), -1);
}

void
message_display(GtkMessageType type, const gchar *fmt, ...)
{
	va_list ap;
	gchar buf[255];

	gtk_info_bar_set_message_type(GTK_INFO_BAR(info_widget), type);

	va_start(ap, fmt);
	g_vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	gtk_label_set_text(GTK_LABEL(message_widget), buf);
}

sptr_t
editor_msg(unsigned int iMessage, uptr_t wParam, sptr_t lParam)
{
	return scintilla_send_message(SCINTILLA(editor_widget),
				      iMessage, wParam, lParam);
}

static gboolean
cmdline_key_pressed(GtkWidget *widget, GdkEventKey *event,
		    gpointer user_data __attribute__((unused)))
{
	gchar key = '\0';

	g_printf("KEY \"%s\" (%d) SHIFT=%d CNTRL=%d\n",
		 event->string, *event->string,
		 event->state & GDK_SHIFT_MASK, event->state & GDK_CONTROL_MASK);

	switch (event->keyval) {
	case GDK_BackSpace:
		key = '\b';
		break;
	case GDK_Tab:
		key = '\t';
		break;
	default:
		key = *event->string;
	}

	if (key)
		cmdline_keypress(key);

	return TRUE;
}

static gboolean
exit_app(GtkWidget *w __attribute__((unused)),
	 GdkEventAny *e __attribute__((unused)),
	 gpointer p __attribute__((unused)))
{
	gtk_main_quit();
	return TRUE;
}

static void
widget_set_font(GtkWidget *widget, const gchar *font_name)
{
	PangoFontDescription *font_desc;

	font_desc = pango_font_description_from_string(font_name);
	gtk_widget_modify_font(widget, font_desc);
	pango_font_description_free(font_desc);
}

int
main(int argc, char **argv)
{
	GtkWidget *window, *vbox;
	GtkWidget *info_content;

	gtk_init(&argc, &argv);

	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(window), "SciTECO");
	g_signal_connect(G_OBJECT(window), "delete-event",
			 G_CALLBACK(exit_app), NULL);

	vbox = gtk_vbox_new(FALSE, 0);

	editor_widget = scintilla_new();
	scintilla_set_id(SCINTILLA(editor_widget), 0);
	gtk_widget_set_usize(editor_widget, 500, 300);
	gtk_widget_set_can_focus(editor_widget, FALSE);
	gtk_box_pack_start(GTK_BOX(vbox), editor_widget, TRUE, TRUE, 0);

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
			 G_CALLBACK(cmdline_key_pressed), NULL);
	gtk_box_pack_start(GTK_BOX(vbox), cmdline_widget, FALSE, FALSE, 0);

	gtk_container_add(GTK_CONTAINER(window), vbox);

	filename_popup = GTK_INFO_POPUP(gtk_info_popup_new(cmdline_widget));

	editor_msg(SCI_SETFOCUS, 1);
	editor_msg(SCI_SETCARETSTYLE, 2);
	editor_msg(SCI_STYLESETFONT, STYLE_DEFAULT, (sptr_t)"Courier");
	editor_msg(SCI_STYLECLEARALL);
	editor_msg(SCI_SETLEXER, SCLEX_CPP);
	editor_msg(SCI_SETKEYWORDS, 0, (sptr_t)"int char");
	editor_msg(SCI_STYLESETFORE, SCE_C_COMMENT, 0x008000);
	editor_msg(SCI_STYLESETFORE, SCE_C_COMMENTLINE, 0x008000);
	editor_msg(SCI_STYLESETFORE, SCE_C_NUMBER, 0x808000);
	editor_msg(SCI_STYLESETFORE, SCE_C_WORD, 0x800000);
	editor_msg(SCI_STYLESETFORE, SCE_C_STRING, 0x800080);
	editor_msg(SCI_STYLESETBOLD, SCE_C_OPERATOR, 1);

	qregisters.initialize();
	ring.edit(NULL);

	undo.enabled = true;

	cmdline_display("*");
	gtk_widget_grab_focus(cmdline_widget);

	gtk_widget_show_all(window);
	gtk_main();

	return 0;
}
