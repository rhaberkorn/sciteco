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

#ifndef __GLOB_H
#define __GLOB_H

#include <string.h>

#include <glib.h>

#include "sciteco.h"
#include "parser.h"

namespace SciTECO {

/*
 * Auxiliary functions
 */
static inline bool
is_glob_pattern(const gchar *str)
{
	return strchr(str, '*') || strchr(str, '?');
}

class Globber {
	gchar *dirname;
	GDir *dir;
	GPatternSpec *pattern;

public:
	Globber(const gchar *pattern);
	~Globber();

	gchar *next(void);
};

/*
 * Command states
 */

class StateGlob : public StateExpectFile {
private:
	State *done(const gchar *str);
};

namespace States {
	extern StateGlob glob;
}

} /* namespace SciTECO */

#endif
