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

#include <glib.h>
#include <glib/gprintf.h>

#include "sciteco.h"
#include "qregisters.h"
#include "interface.h"
#include "cmdline.h"

namespace SciTECO {

ReplaceCmdline::ReplaceCmdline()
{
	QRegister *cmdline_reg = QRegisters::globals["\x1B"];

	new_cmdline = cmdline_reg->get_string();
	for (pos = 0; cmdline[pos] && cmdline[pos] == new_cmdline[pos]; pos++);
	pos++;
}

Error::Frame *
Error::QRegFrame::copy() const
{
	Frame *frame = new QRegFrame(name);

	frame->pos = pos;
	frame->line = line;
	frame->column = column;

	return frame;
}

void
Error::QRegFrame::display(gint nr)
{
	interface.msg(InterfaceCurrent::MSG_INFO,
		      "#%d in Q-Register \"%s\" at %d (%d:%d)",
		      nr, name, pos, line, column);
}

Error::Frame *
Error::FileFrame::copy() const
{
	Frame *frame = new FileFrame(name);

	frame->pos = pos;
	frame->line = line;
	frame->column = column;

	return frame;
}

void
Error::FileFrame::display(gint nr)
{
	interface.msg(InterfaceCurrent::MSG_INFO,
		      "#%d in file \"%s\" at %d (%d:%d)",
		      nr, name, pos, line, column);
}

Error::Frame *
Error::EDHookFrame::copy() const
{
	/* coordinates do not matter */
	return new EDHookFrame(type);
}

void
Error::EDHookFrame::display(gint nr)
{
	interface.msg(InterfaceCurrent::MSG_INFO,
	              "#%d in \"%s\" hook execution",
	              nr, type);
}

Error::Frame *
Error::ToplevelFrame::copy() const
{
	Frame *frame = new ToplevelFrame();

	frame->pos = pos;
	frame->line = line;
	frame->column = column;

	return frame;
}

void
Error::ToplevelFrame::display(gint nr)
{
	interface.msg(InterfaceCurrent::MSG_INFO,
		      "#%d in toplevel macro at %d (%d:%d)",
		      nr, pos, line, column);
}

Error::Error(const gchar *fmt, ...)
            : frames(NULL), pos(0), line(0), column(0)
{
	va_list ap;

	va_start(ap, fmt);
	description = g_strdup_vprintf(fmt, ap);
	va_end(ap);
}

Error::Error(const Error &inst)
            : description(g_strdup(inst.description)),
              pos(inst.pos), line(inst.line), column(inst.column)
{
	/* shallow copy of the frames */
	frames = g_slist_copy(inst.frames);

	for (GSList *cur = frames; cur; cur = g_slist_next(cur)) {
		Frame *frame = (Frame *)cur->data;
		cur->data = frame->copy();
	}
}

void
Error::add_frame(Frame *frame)
{
	frame->pos = pos;
	frame->line = line;
	frame->column = column;

	frames = g_slist_prepend(frames, frame);
}

void
Error::display_short(void)
{
	interface.msg(InterfaceCurrent::MSG_ERROR,
		      "%s (at %d)", description, pos);
}

void
Error::display_full(void)
{
	gint nr = 0;

	interface.msg(InterfaceCurrent::MSG_ERROR, "%s", description);

	frames = g_slist_reverse(frames);
	for (GSList *cur = frames; cur; cur = g_slist_next(cur)) {
		Frame *frame = (Frame *)cur->data;

		frame->display(nr++);
	}
}

Error::~Error()
{
	g_free(description);
	for (GSList *cur = frames; cur; cur = g_slist_next(cur))
		delete (Frame *)cur->data;
	g_slist_free(frames);
}

} /* namespace SciTECO */
