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

#ifndef __PARSER_H
#define __PARSER_H

#include <glib.h>
#include <glib/gprintf.h>

#include "sciteco.h"

/* TECO uses only lower 7 bits for commands */
#define MAX_TRANSITIONS	127

class State {
public:
	class Error {
	public:
		Error(const gchar *fmt, ...);
	};

	class SyntaxError : public Error {
	public:
		SyntaxError(gchar chr)
			   : Error("Syntax error \"%c\" (%d)", chr, chr) {}
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

protected:
	/* static transitions */
	State *transitions[MAX_TRANSITIONS];

	inline void
	init(const gchar *chars, State &state)
	{
		while (*chars)
			transitions[(int)*chars++] = &state;
	}
	inline void
	init(const gchar *chars)
	{
		init(chars, *this);
	}

public:
	State();

	static void input(gchar chr) throw (Error);
	State *get_next_state(gchar chr) throw (Error);

protected:
	static bool eval_colon(void);

	virtual State *
	custom(gchar chr) throw (Error)
	{
		throw SyntaxError(chr);
		return NULL;
	}
};

/*
 * Super-class for states accepting string arguments
 * Opaquely cares about alternative-escape characters,
 * string building commands and accumulation into a string
 */
class StateExpectString : public State {
	struct Machine {
		enum State {
			STATE_START,
			STATE_ESCAPED,
			STATE_LOWER,
			STATE_UPPER,
			STATE_CTL_E,
			STATE_CTL_EQ,
			STATE_CTL_EQ_LOCAL,
			STATE_CTL_EU,
			STATE_CTL_EU_LOCAL
		} state;

		enum Mode {
			MODE_NORMAL,
			MODE_UPPER,
			MODE_LOWER
		} mode;

		bool toctl;

		Machine() : state(STATE_START),
			    mode(MODE_NORMAL), toctl(false) {}
	} machine;

	gint nesting;

	bool string_building;
	bool last;

public:
	StateExpectString(bool _building = true, bool _last = true)
			 : State(), nesting(1),
			   string_building(_building), last(_last) {}

private:
	gchar *machine_input(gchar key) throw (Error);
	State *custom(gchar chr) throw (Error);

protected:
	virtual void initial(void) throw (Error) {}
	virtual void process(const gchar *str, gint new_chars) throw (Error) {}
	virtual State *done(const gchar *str) throw (Error) = 0;
};

class QRegister;

/*
 * Super class for states accepting Q-Register specifications
 */
class StateExpectQReg : public State {
	bool got_local;

public:
	StateExpectQReg();

private:
	State *custom(gchar chr) throw (Error);

protected:
	/*
	 * FIXME: would be nice to pass reg as reference, but there are
	 * circular header dependencies...
	 */
	virtual State *got_register(QRegister *reg) throw (Error) = 0;
};

class StateStart : public State {
public:
	StateStart();

private:
	void insert_integer(gint64 v);
	gint64 read_integer(void);

	tecoBool move_chars(gint64 n);
	tecoBool move_lines(gint64 n);

	tecoBool delete_words(gint64 n);

	State *custom(gchar chr) throw (Error);
};

class StateControl : public State {
public:
	StateControl();

private:
	State *custom(gchar chr) throw (Error);
};

class StateFCommand : public State {
public:
	StateFCommand();

private:
	State *custom(gchar chr) throw (Error);
};

class StateCondCommand : public State {
public:
	StateCondCommand();

private:
	State *custom(gchar chr) throw (Error);
};

class StateECommand : public State {
public:
	StateECommand();

private:
	State *custom(gchar chr) throw (Error);
};

class StateScintilla_symbols : public StateExpectString {
public:
	StateScintilla_symbols() : StateExpectString(true, false) {}

private:
	State *done(const gchar *str) throw (Error);
};

class StateScintilla_lParam : public StateExpectString {
private:
	State *done(const gchar *str) throw (Error);
};

/*
 * also serves as base class for replace-insertion states
 */
class StateInsert : public StateExpectString {
protected:
	void initial(void) throw (Error);
	void process(const gchar *str, gint new_chars) throw (Error);
	State *done(const gchar *str) throw (Error);
};

namespace States {
	extern StateStart 		start;
	extern StateControl		control;
	extern StateFCommand		fcommand;
	extern StateCondCommand		condcommand;
	extern StateECommand		ecommand;
	extern StateScintilla_symbols	scintilla_symbols;
	extern StateScintilla_lParam	scintilla_lparam;
	extern StateInsert		insert;

	extern State *current;
}

extern enum Mode {
	MODE_NORMAL = 0,
	MODE_PARSE_ONLY_GOTO,
	MODE_PARSE_ONLY_LOOP,
	MODE_PARSE_ONLY_COND
} mode;

#define BEGIN_EXEC(STATE) G_STMT_START {	\
	if (mode > MODE_NORMAL)			\
		return STATE;			\
} G_STMT_END

extern gint macro_pc;

extern gchar *strings[2];
extern gchar escape_char;

namespace Execute {
	void step(const gchar *macro) throw (State::Error);
	void macro(const gchar *macro, bool locals = true) throw (State::Error);
	bool file(const gchar *filename, bool locals = true);
}

#endif
