#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>

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

#define INI_FILE ".teco_ini"

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

	va_start(ap, fmt);
	g_vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	switch (type) {
	case GTK_MESSAGE_ERROR:
		g_fprintf(stderr, "Error: %s\n", buf);
		break;
	case GTK_MESSAGE_WARNING:
		g_fprintf(stderr, "Warning: %s\n", buf);
		break;
	case GTK_MESSAGE_INFO:
		g_printf("Info: %s\n", buf);
		break;
	default:
		g_printf("%s\n", buf);
	}

	gtk_info_bar_set_message_type(GTK_INFO_BAR(info_widget), type);
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

#ifdef DEBUG
	g_printf("KEY \"%s\" (%d) SHIFT=%d CNTRL=%d\n",
		 event->string, *event->string,
		 event->state & GDK_SHIFT_MASK, event->state & GDK_CONTROL_MASK);
#endif

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

static gchar *mung_file = NULL;

static inline void
process_options(int &argc, char **&argv)
{
	static const GOptionEntry option_entries[] = {
		{"mung", 'm', 0, G_OPTION_ARG_FILENAME, &mung_file,
		 "Mung file instead of " INI_FILE, "filename"},
		{NULL}
	};

	GOptionContext *options;

	options = g_option_context_new("- Advanced interactive TECO");
	g_option_context_add_main_entries(options, option_entries, NULL);
	g_option_context_add_group(options, gtk_get_option_group(TRUE));
	if (!g_option_context_parse(options, &argc, &argv, NULL)) {
		g_printf("Option parsing failed!\n");
		exit(EXIT_FAILURE);
	}
	g_option_context_free(options);

	if (mung_file) {
		if (!g_file_test(mung_file, G_FILE_TEST_IS_REGULAR)) {
			g_printf("Cannot mung %s. File does not exist!\n",
				 mung_file);
			exit(EXIT_FAILURE);
		}
	} else {
		mung_file = g_build_filename(g_get_user_config_dir(),
					     INI_FILE, NULL);
	}

	gtk_init(&argc, &argv);

	/* remaining arguments, are arguments to the munged file */
}

int
main(int argc, char **argv)
{
	GtkWidget *window, *vbox;
	GtkWidget *info_content;

	process_options(argc, argv);

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

	/* add remaining arguments to unnamed buffer */
	for (int i = 1; i < argc; i++) {
		editor_msg(SCI_ADDTEXT, strlen(argv[i]), (sptr_t)argv[i]);
		editor_msg(SCI_ADDTEXT, 1, (sptr_t)"\r");
	}
	editor_msg(SCI_GOTOPOS, 0);

	if (g_file_test(mung_file, G_FILE_TEST_IS_REGULAR)) {
		if (!file_execute(mung_file))
			exit(EXIT_FAILURE);
		/* FIXME: make quit immediate in commandline mode (non-UNDO)? */
		if (quit_requested) {
			/* FIXME */
			exit(EXIT_SUCCESS);
		}
	}
	g_free(mung_file);

	undo.enabled = true;

	cmdline_display("*");
	gtk_widget_grab_focus(cmdline_widget);

	gtk_widget_show_all(window);
	gtk_main();

	return 0;
}
