#ifndef __INTERFACE_H
#define __INTERFACE_H

#include <stdarg.h>

#include <glib.h>

#include <Scintilla.h>

/*
 * Base class for all user interfaces - used mereley as a class interface.
 * The actual instance of the interface has the platform-specific type
 * (e.g. InterfaceGtk) since we would like to have the benefits of using
 * classes but avoid the calling overhead when invoking virtual methods
 * on Interface pointers.
 * There's only one Interface* instance in the system.
 */
class Interface {
public:
	virtual GOptionGroup *
	get_options(void)
	{
		return NULL;
	}
	virtual void parse_args(int &argc, char **&argv) {}

	enum MessageType {
		MSG_USER,
		MSG_INFO,
		MSG_WARNING,
		MSG_ERROR
	};
	virtual void vmsg(MessageType type, const gchar *fmt, va_list ap) = 0;
	inline void
	msg(MessageType type, const gchar *fmt, ...) G_GNUC_PRINTF(3, 4)
	{
		va_list ap;

		va_start(ap, fmt);
		vmsg(type, fmt, ap);
		va_end(ap);
	}

	virtual sptr_t ssm(unsigned int iMessage,
			   uptr_t wParam = 0, sptr_t lParam = 0) = 0;

	virtual void cmdline_update(const gchar *cmdline = "") = 0;

	enum PopupFileType {
		POPUP_FILE,
		POPUP_DIRECTORY
	};
	virtual void popup_add_filename(PopupFileType type,
					const gchar *filename,
		     			bool highlight = false) = 0;
	virtual void popup_show(void) = 0;
	virtual void popup_clear(void) = 0;

	/* main entry point */
	virtual void event_loop(void) = 0;

protected:
	/* see main.cpp */
	void stdio_vmsg(MessageType type, const gchar *fmt, va_list ap);
};

#ifdef INTERFACE_GTK
#include "interface-gtk.h"
#elif defined(INTERFACE_NCURSES)
#include "interface-ncurses.h"
#endif

#endif
