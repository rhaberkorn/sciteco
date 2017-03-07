/*
 * Copyright (C) 2012-2017 Robin Haberkorn
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

#ifndef __CMDLINE_H
#define __CMDLINE_H

#include <glib.h>

#include "memory.h"
#include "parser.h"
#include "qregisters.h"
#include "undo.h"

namespace SciTECO {

/*
 * NOTE: Some of the members (esp. insert() and rubout())
 * have to be public, so that State::process_edit_cmd()
 * implementations can access it.
 * Otherwise, we'd have to list all implementations as
 * friend methods, which is inelegant.
 */
extern class Cmdline : public Object {
public:
	/**
	 * String containing the current command line.
	 * It is not null-terminated and contains the effective
	 * command-line up to cmdline_len followed by the recently rubbed-out
	 * command-line of length cmdline_rubout_len.
	 */
	gchar *str;
	/** Effective command line length */
	gsize len;
	/** Length of the rubbed out command line */
	gsize rubout_len;
	/** Program counter within the command-line macro */
	guint pc;

	Cmdline() : str(NULL), len(0), rubout_len(0), pc(0) {}
	inline
	~Cmdline()
	{
		g_free(str);
	}

	inline gchar
	operator [](guint i) const
	{
		return str[i];
	}

	void keypress(gchar key);
	inline void
	keypress(const gchar *keys)
	{
		while (*keys)
			keypress(*keys++);
	}

	void fnmacro(const gchar *name);

	void replace(void) G_GNUC_NORETURN;

	inline void
	rubout(void)
	{
		if (len) {
			undo.pop(--len);
			rubout_len++;
		}
	}

	void insert(const gchar *src = NULL);
	inline void
	insert(gchar key)
	{
		gchar src[] = {key, '\0'};
		insert(src);
	}
} cmdline;

extern bool quit_requested;

/*
 * Command states
 */

class StateSaveCmdline : public StateExpectQReg {
public:
	StateSaveCmdline() : StateExpectQReg(QREG_OPTIONAL_INIT) {}

private:
	State *got_register(QRegister *reg);
};

namespace States {
	extern StateSaveCmdline save_cmdline;
}

} /* namespace SciTECO */

#endif
