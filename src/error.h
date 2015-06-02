/*
 * Copyright (C) 2012-2015 Robin Haberkorn
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

#ifndef __ERROR_H
#define __ERROR_H

#include <exception>
#include <typeinfo>

#include <glib.h>
#include <glib/gprintf.h>

#include "sciteco.h"
#include "string-utils.h"

namespace SciTECO {

/*
 * Thrown as exception to signify that program
 * should be terminated.
 */
class Quit {};

class Error {
	gchar *description;
	GSList *frames;

public:
	gint pos;
	gint line, column;

	class Frame {
	public:
		gint pos;
		gint line, column;

		virtual Frame *copy() const = 0;
		virtual ~Frame() {}

		virtual void display(gint nr) = 0;
	};

	class QRegFrame : public Frame {
		gchar *name;

	public:
		QRegFrame(const gchar *_name)
			 : name(g_strdup(_name)) {}

		Frame *copy() const;

		~QRegFrame()
		{
			g_free(name);
		}

		void display(gint nr);
	};

	class FileFrame : public Frame {
		gchar *name;

	public:
		FileFrame(const gchar *_name)
			 : name(g_strdup(_name)) {}

		Frame *copy() const;

		~FileFrame()
		{
			g_free(name);
		}

		void display(gint nr);
	};

	class EDHookFrame : public Frame {
		gchar *type;

	public:
		EDHookFrame(const gchar *_type)
		           : type(g_strdup(_type)) {}

		Frame *copy() const;

		~EDHookFrame()
		{
			g_free(type);
		}

		void display(gint nr);
	};

	class ToplevelFrame : public Frame {
	public:
		Frame *copy() const;

		void display(gint nr);
	};

	Error(const gchar *fmt, ...) G_GNUC_PRINTF(2, 3);
	Error(const Error &inst);
	~Error();

	inline void
	set_coord(const gchar *str, gint _pos)
	{
		pos = _pos;
		String::get_coord(str, pos, line, column);
	}

	void add_frame(Frame *frame);

	void display_short(void);
	void display_full(void);
};

class StdError : public Error {
public:
	StdError(const gchar *type, const std::exception &error)
		: Error("%s: %s", type, error.what()) {}
	StdError(const std::exception &error)
		: Error("%s: %s", typeid(error).name(), error.what()) {}
};

class GlibError : public Error {
public:
	/**
	 * Construct error for glib's GError.
	 * Ownership of the error's resources is passed
	 * the GlibError object.
	 */
	GlibError(GError *gerror)
	         : Error("%s", gerror->message)
	{
		g_error_free(gerror);
	}
};

class SyntaxError : public Error {
public:
	SyntaxError(gchar chr)
		   : Error("Syntax error \"%c\" (%d)", chr, chr) {}
};

class ArgExpectedError : public Error {
public:
	ArgExpectedError(const gchar *cmd)
		        : Error("Argument expected for <%s>", cmd) {}
	ArgExpectedError(gchar cmd)
		        : Error("Argument expected for <%c>", cmd) {}
};

class MoveError : public Error {
public:
	MoveError(const gchar *cmd)
		 : Error("Attempt to move pointer off page with <%s>",
			 cmd) {}
	MoveError(gchar cmd)
		 : Error("Attempt to move pointer off page with <%c>",
			 cmd) {}
};

class RangeError : public Error {
public:
	RangeError(const gchar *cmd)
		  : Error("Invalid range specified for <%s>", cmd) {}
	RangeError(gchar cmd)
		  : Error("Invalid range specified for <%c>", cmd) {}
};

class InvalidQRegError : public Error {
public:
	InvalidQRegError(const gchar *name, bool local = false)
			: Error("Invalid Q-Register \"%s%s\"",
				local ? "." : "", name) {}
	InvalidQRegError(gchar name, bool local = false)
			: Error("Invalid Q-Register \"%s%c\"",
				local ? "." : "", name) {}
};

class QRegOpUnsupportedError : public Error {
public:
	QRegOpUnsupportedError(const gchar *name, bool local = false)
	                      : Error("Operation unsupported on "
	                              "Q-Register \"%s%s\"",
	                              local ? "." : "", name) {}
};

} /* namespace SciTECO */

#endif
