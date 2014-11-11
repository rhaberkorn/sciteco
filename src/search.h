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

#ifndef __SEARCH_H
#define __SEARCH_H

#include <glib.h>

#include "sciteco.h"
#include "parser.h"
#include "ring.h"
#include "qregisters.h"

namespace SciTECO {

/*
 * "S" command state and base class for all other search/replace commands
 */
class StateSearch : public StateExpectString {
public:
	StateSearch(bool last = true) : StateExpectString(true, last) {}

protected:
	struct Parameters {
		gint dot;
		gint from, to;
		gint count;

		Buffer *from_buffer, *to_buffer;
	} parameters;

	QRegSpecMachine qreg_machine;

	enum MatchState {
		STATE_START,
		STATE_NOT,
		STATE_CTL_E,
		STATE_ANYQ,
		STATE_MANY,
		STATE_ALT
	};

	gchar *class2regexp(MatchState &state, const gchar *&pattern,
			    bool escape_default = false);
	gchar *pattern2regexp(const gchar *&pattern, bool single_expr = false);
	void do_search(GRegex *re, gint from, gint to, gint &count);

	virtual void initial(void);
	virtual void process(const gchar *str, gint new_chars);
	virtual State *done(const gchar *str);
};

class StateSearchAll : public StateSearch {
private:
	void initial(void);
	State *done(const gchar *str);
};

class StateSearchKill : public StateSearch {
private:
	State *done(const gchar *str);
};

class StateSearchDelete : public StateSearch {
public:
	StateSearchDelete(bool last = true) : StateSearch(last) {}

protected:
	State *done(const gchar *str);
};

class StateReplace : public StateSearchDelete {
public:
	StateReplace() : StateSearchDelete(false) {}

private:
	State *done(const gchar *str);
};

class StateReplace_insert : public StateInsert {
private:
	void initial(void) {}
};

class StateReplace_ignore : public StateExpectString {
private:
	State *done(const gchar *str);
};

class StateReplaceDefault : public StateSearchDelete {
public:
	StateReplaceDefault() : StateSearchDelete(false) {}

private:
	State *done(const gchar *str);
};

class StateReplaceDefault_insert : public StateInsert {
private:
	void initial(void) {}
	State *done(const gchar *str);
};

class StateReplaceDefault_ignore : public StateExpectString {
private:
	State *done(const gchar *str);
};

namespace States {
	extern StateSearch			search;
	extern StateSearchAll			searchall;
	extern StateSearchKill			searchkill;
	extern StateSearchDelete			searchdelete;

	extern StateReplace			replace;
	extern StateReplace_insert		replace_insert;
	extern StateReplace_ignore		replace_ignore;

	extern StateReplaceDefault		replacedefault;
	extern StateReplaceDefault_insert		replacedefault_insert;
	extern StateReplaceDefault_ignore		replacedefault_ignore;
}

} /* namespace SciTECO */

#endif
