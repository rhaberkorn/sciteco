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

#ifndef __INTERFACE_H
#define __INTERFACE_H

#include <stdarg.h>
#include <signal.h>

#include <glib.h>

#include <Scintilla.h>

#include "undo.h"

namespace SciTECO {

/* avoid include dependency conflict */
class QRegister;
class Buffer;
extern sig_atomic_t sigint_occurred;

/**
 * Base class for all SciTECO views. This is a minimal
 * abstraction where the implementor only has to provide
 * a method for dispatching Scintilla messages.
 * Everything else is handled by other SciTECO classes.
 *
 * This interface employs the Curiously Recurring Template
 * Pattern (CRTP). To implement it, one must derive from
 * View<DerivedClass>. The methods to implement actually
 * have an "_impl" suffix so as to avoid infinite recursion
 * if an implementation is missing.
 * Externally however, the methods as given in this interface
 * may be called.
 *
 * The CRTP has a runtime overhead at low optimization levels
 * (additional non-inlined calls), but should provide a
 * significant performance boost when inlining is enabled.
 *
 * Note that not all methods have to be defined in the
 * class. Explicit template instantiation is used to outsource
 * base-class implementations to interface.cpp.
 */
template <class ViewImpl>
class View {
	inline ViewImpl &
	impl(void)
	{
		return *(ViewImpl *)this;
	}

	class UndoTokenMessage : public UndoToken {
		ViewImpl &view;

		unsigned int iMessage;
		uptr_t wParam;
		sptr_t lParam;

	public:
		UndoTokenMessage(ViewImpl &_view, unsigned int _iMessage,
				 uptr_t _wParam = 0, sptr_t _lParam = 0)
				: UndoToken(), view(_view),
		                  iMessage(_iMessage),
				  wParam(_wParam), lParam(_lParam) {}

		void
		run(void)
		{
			view.ssm(iMessage, wParam, lParam);
		}
	};

	class UndoTokenSetRepresentations : public UndoToken {
		ViewImpl &view;

	public:
		UndoTokenSetRepresentations(ViewImpl &_view)
		                           : view(_view) {}

		void
		run(void)
		{
			view.set_representations();
		}
	};

public:
	/*
	 * called after Interface initialization.
	 * Should call setup()
	 */
	inline void
	initialize(void)
	{
		impl().initialize_impl();
	}

	inline sptr_t
	ssm(unsigned int iMessage,
	    uptr_t wParam = 0, sptr_t lParam = 0)
	{
		return impl().ssm_impl(iMessage, wParam, lParam);
	}

	inline void
	undo_ssm(unsigned int iMessage,
		 uptr_t wParam = 0, sptr_t lParam = 0)
	{
		undo.push(new UndoTokenMessage(impl(), iMessage, wParam, lParam));
	}

	void set_representations(void);
	inline void
	undo_set_representations(void)
	{
		undo.push(new UndoTokenSetRepresentations(impl()));
	}

protected:
	void setup(void);
};

/**
 * Base class and interface of all SciTECO user interfaces
 * (e.g. Curses or GTK+).
 *
 * This uses the same Curiously Recurring Template Pattern (CRTP)
 * as the View interface above, as there is only one type of
 * user interface at runtime.
 */
template <class InterfaceImpl, class ViewImpl>
class Interface {
	inline InterfaceImpl &
	impl(void)
	{
		return *(InterfaceImpl *)this;
	}

	class UndoTokenShowView : public UndoToken {
		ViewImpl *view;

	public:
		UndoTokenShowView(ViewImpl *_view)
		                 : view(_view) {}

		void run(void);
	};

	template <class Type>
	class UndoTokenInfoUpdate : public UndoToken {
		Type *obj;

	public:
		UndoTokenInfoUpdate(Type *_obj)
				   : obj(_obj) {}

		void run(void);
	};

protected:
	ViewImpl *current_view;

public:
	Interface() : current_view(NULL) {}

	/* default implementation */
	inline GOptionGroup *
	get_options(void)
	{
		return NULL;
	}

	/* expected to initialize Scintilla */
	inline void
	main(int &argc, char **&argv)
	{
		impl().main_impl(argc, argv);
	}

	enum MessageType {
		MSG_USER,
		MSG_INFO,
		MSG_WARNING,
		MSG_ERROR
	};
	inline void
	vmsg(MessageType type, const gchar *fmt, va_list ap)
	{
		impl().vmsg_impl(type, fmt, ap);
	}
	inline void
	msg(MessageType type, const gchar *fmt, ...) G_GNUC_PRINTF(3, 4)
	{
		va_list ap;

		va_start(ap, fmt);
		vmsg(type, fmt, ap);
		va_end(ap);
	}
	/* default implementation */
	inline void msg_clear(void) {}

	inline void
	show_view(ViewImpl *view)
	{
		impl().show_view_impl(view);
	}
	inline void
	undo_show_view(ViewImpl *view)
	{
		undo.push(new UndoTokenShowView(view));
	}

	inline ViewImpl *
	get_current_view(void)
	{
		return current_view;
	}

	inline sptr_t
	ssm(unsigned int iMessage, uptr_t wParam = 0, sptr_t lParam = 0)
	{
		return current_view->ssm(iMessage, wParam, lParam);
	}
	inline void
	undo_ssm(unsigned int iMessage,
	         uptr_t wParam = 0, sptr_t lParam = 0)
	{
		current_view->undo_ssm(iMessage, wParam, lParam);
	}

	/*
	 * NOTE: could be rolled into a template, but
	 * this way it is explicit what must be implemented
	 * by the deriving class.
	 */
	inline void
	info_update(QRegister *reg)
	{
		impl().info_update_impl(reg);
	}
	inline void
	info_update(Buffer *buffer)
	{
		impl().info_update_impl(buffer);
	}

	inline void
	undo_info_update(QRegister *reg)
	{
		undo.push(new UndoTokenInfoUpdate<QRegister>(reg));
	}
	inline void
	undo_info_update(Buffer *buffer)
	{
		undo.push(new UndoTokenInfoUpdate<Buffer>(buffer));
	}

	/* NULL means to redraw the current cmdline if necessary */
	inline void
	cmdline_update(const gchar *cmdline = NULL)
	{
		impl().cmdline_update_impl(cmdline);
	}

	enum PopupEntryType {
		POPUP_PLAIN,
		POPUP_FILE,
		POPUP_DIRECTORY
	};
	inline void
	popup_add(PopupEntryType type,
	          const gchar *name, bool highlight = false)
	{
		impl().popup_add_impl(type, name, highlight);
	}
	inline void
	popup_show(void)
	{
		impl().popup_show_impl();
	}
	inline void
	popup_clear(void)
	{
		impl().popup_clear_impl();
	}

	/* default implementation */
	inline bool
	is_interrupted(void)
	{
		return sigint_occurred != FALSE;
	}

	/* main entry point */
	inline void
	event_loop(void)
	{
		impl().event_loop_impl();
	}

	/*
	 * Interfacing to the external SciTECO world
	 */
protected:
	void stdio_vmsg(MessageType type, const gchar *fmt, va_list ap);
public:
	void process_notify(SCNotification *notify);
};

} /* namespace SciTECO */

#ifdef INTERFACE_GTK
#include "interface-gtk.h"
#elif defined(INTERFACE_CURSES)
#include "interface-curses.h"
#else
#error No interface selected!
#endif

namespace SciTECO {

/* object defined in main.cpp */
extern InterfaceCurrent interface;

extern template class View<ViewCurrent>;
extern template class Interface<InterfaceCurrent, ViewCurrent>;

} /* namespace SciTECO */

#endif
