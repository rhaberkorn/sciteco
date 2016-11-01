/*
 * Copyright (C) 2012-2016 Robin Haberkorn
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

#ifndef __GLOB_H
#define __GLOB_H

#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "sciteco.h"
#include "parser.h"

namespace SciTECO {

class Globber {
	GFileTest test;
	gchar *dirname;
	GDir *dir;
	GRegex *pattern;

public:
	Globber(const gchar *pattern,
	        GFileTest test = G_FILE_TEST_EXISTS);
	~Globber();

	gchar *next(void);

	static inline bool
	is_pattern(const gchar *str)
	{
		return str && strpbrk(str, "*?[") != NULL;
	}

	static gchar *escape_pattern(const gchar *pattern);
	static GRegex *compile_pattern(const gchar *pattern);
};

/*
 * Command states
 */

class StateGlob_pattern : public StateExpectFile {
public:
	StateGlob_pattern() : StateExpectFile(true, false) {}

private:
	State *got_file(const gchar *filename);
};

class StateGlob_filename : public StateExpectFile {
private:
	State *got_file(const gchar *filename);
};

namespace States {
	extern StateGlob_pattern	glob_pattern;
	extern StateGlob_filename	glob_filename;
}

} /* namespace SciTECO */

#endif
