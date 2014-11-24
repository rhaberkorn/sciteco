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
#include <stdio.h>

#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>

#include <Scintilla.h>
#include <SciLexer.h>

#include "sciteco.h"
#include "interface.h"

namespace SciTECO {

template <class ViewImpl>
void
View<ViewImpl>::set_representations(void)
{
	static const char *reps[] = {
		"^@", "^A", "^B", "^C", "^D", "^E", "^F", "^G",
		"^H", "TAB" /* ^I */, "LF" /* ^J */, "^K", "^L", "CR" /* ^M */, "^N", "^O",
		"^P", "^Q", "^R", "^S", "^T", "^U", "^V", "^W",
		"^X", "^Y", "^Z", "$" /* ^[ */, "^\\", "^]", "^^", "^_"
	};

	for (guint cc = 0; cc < G_N_ELEMENTS(reps); cc++) {
		gchar buf[] = {(gchar)cc, '\0'};
		ssm(SCI_SETREPRESENTATION, (uptr_t)buf, (sptr_t)reps[cc]);
	}
}

template <class ViewImpl>
void
View<ViewImpl>::setup(void)
{
	/*
	 * Start with or without undo collection,
	 * depending on undo.enabled.
	 */
	ssm(SCI_SETUNDOCOLLECTION, undo.enabled);

	ssm(SCI_SETFOCUS, TRUE);

	ssm(SCI_SETCARETSTYLE, CARETSTYLE_BLOCK);
	ssm(SCI_SETCARETFORE, 0xFFFFFF);

	/*
	 * FIXME: Default styles should probably be set interface-based
	 * (system defaults)
	 */
	ssm(SCI_STYLESETFORE, STYLE_DEFAULT, 0xFFFFFF);
	ssm(SCI_STYLESETBACK, STYLE_DEFAULT, 0x000000);
	ssm(SCI_STYLESETFONT, STYLE_DEFAULT, (sptr_t)"Courier");
	ssm(SCI_STYLECLEARALL);

	ssm(SCI_STYLESETBACK, STYLE_LINENUMBER, 0x000000);
}

template class View<ViewCurrent>;

template <class InterfaceImpl, class ViewImpl>
void
Interface<InterfaceImpl, ViewImpl>::UndoTokenShowView::run(void)
{
	/*
	 * Implementing this here allows us to reference
	 * `interface`
	 */
	interface.show_view(view);
}

template <class InterfaceImpl, class ViewImpl>
template <class Type>
void
Interface<InterfaceImpl, ViewImpl>::UndoTokenInfoUpdate<Type>::run(void)
{
	interface.info_update(obj);
}

template <class InterfaceImpl, class ViewImpl>
void
Interface<InterfaceImpl, ViewImpl>::stdio_vmsg(MessageType type, const gchar *fmt, va_list ap)
{
	gchar buf[255];

	g_vsnprintf(buf, sizeof(buf), fmt, ap);

	switch (type) {
	case MSG_USER:
		g_printf("%s\n", buf);
		break;
	case MSG_INFO:
		g_printf("Info: %s\n", buf);
		break;
	case MSG_WARNING:
		g_fprintf(stderr, "Warning: %s\n", buf);
		break;
	case MSG_ERROR:
		g_fprintf(stderr, "Error: %s\n", buf);
		break;
	}
}

template <class InterfaceImpl, class ViewImpl>
void
Interface<InterfaceImpl, ViewImpl>::process_notify(SCNotification *notify)
{
#ifdef DEBUG
	g_printf("SCINTILLA NOTIFY: code=%d\n", notify->nmhdr.code);
#endif
}

template class Interface<InterfaceCurrent, ViewCurrent>;

} /* namespace SciTECO */
