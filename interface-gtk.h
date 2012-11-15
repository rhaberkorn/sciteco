#ifndef __INTERFACE_GTK_H
#define __INTERFACE_GTK_H

#include <glib.h>
#include <gtk/gtk.h>

#include <Scintilla.h>
#include <ScintillaWidget.h>

#include "interface.h"

extern class InterfaceGtk : public Interface {
	GtkWidget *editor_widget;
	GtkWidget *cmdline_widget;
	GtkWidget *info_widget, *message_widget;

	GtkWidget *popup_widget;

public:
	InterfaceGtk();
	//~InterfaceGtk();

	inline void
	parse_args(int &argc, char **&argv)
	{
		gtk_parse_args(&argc, &argv);
	}

	void msg(MessageType type, const gchar *fmt, ...) G_GNUC_PRINTF(3, 4);

	inline sptr_t
	ssm(unsigned int iMessage, uptr_t wParam = 0, sptr_t lParam = 0)
	{
		return scintilla_send_message(SCINTILLA(editor_widget),
					      iMessage, wParam, lParam);
	}

	void cmdline_update(const gchar *cmdline);

	void popup_add_filename(PopupFileType type,
				const gchar *filename, bool highlight = false);
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
		gtk_main();
	}

private:
	static void widget_set_font(GtkWidget *widget, const gchar *font_name);
} interface;

#endif
