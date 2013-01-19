/*
 * Copyright (C) 2012-2013 Robin Haberkorn
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

#ifndef __INTERFACE_H
#define __INTERFACE_H

#include <stdarg.h>
#include <signal.h>

#include <glib.h>

#include <Scintilla.h>

#include "undo.h"

/* avoid include dependency conflict */
class QRegister;
class Buffer;
extern sig_atomic_t sigint_occurred;

/*
 * Base class for all user interfaces - used mereley as a class interface.
 * The actual instance of the interface has the platform-specific type
 * (e.g. InterfaceGtk) since we would like to have the benefits of using
 * classes but avoid the calling overhead when invoking virtual methods
 * on Interface pointers.
 * There's only one Interface* instance in the system.
 */
class Interface {
	template <class Type>
	class UndoTokenInfoUpdate : public UndoToken {
		Interface *iface;
		Type *obj;

	public:
		UndoTokenInfoUpdate(Interface *_iface, Type *_obj)
				   : iface(_iface), obj(_obj) {}

		void
		run(void)
		{
			iface->info_update(obj);
		}
	};

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
	virtual void msg_clear(void) {}

	virtual sptr_t ssm(unsigned int iMessage,
			   uptr_t wParam = 0, sptr_t lParam = 0) = 0;

	virtual void info_update(QRegister *reg) = 0;
	virtual void info_update(Buffer *buffer) = 0;

	template <class Type>
	inline void
	undo_info_update(Type *obj)
	{
		undo.push(new UndoTokenInfoUpdate<Type>(this, obj));
	}

	/* NULL means to redraw the current cmdline if necessary */
	virtual void cmdline_update(const gchar *cmdline = NULL) = 0;

	enum PopupEntryType {
		POPUP_PLAIN,
		POPUP_FILE,
		POPUP_DIRECTORY
	};
	virtual void popup_add(PopupEntryType type,
			       const gchar *name, bool highlight = false) = 0;
	virtual void popup_show(void) = 0;
	virtual void popup_clear(void) = 0;

	virtual inline bool
	is_interrupted(void)
	{
		return sigint_occurred != FALSE;
	}

	/* main entry point */
	virtual void event_loop(void) = 0;

	/*
	 * Interfacing to the external SciTECO world
	 * See main.cpp
	 */
protected:
	void stdio_vmsg(MessageType type, const gchar *fmt, va_list ap);
public:
	void process_notify(SCNotification *notify);
};

#ifdef INTERFACE_GTK
#include "interface-gtk.h"
#elif defined(INTERFACE_NCURSES)
#include "interface-ncurses.h"
#endif

#endif
