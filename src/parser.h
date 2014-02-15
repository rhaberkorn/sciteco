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

#include "undo.h"
#include "sciteco.h"

/* TECO uses only lower 7 bits for commands */
#define MAX_TRANSITIONS	127

/* thrown as exception, executed at cmdline macro level */
class ReplaceCmdline {
public:
	gchar *new_cmdline;
	gint pos;

	ReplaceCmdline();
};

class State {
public:
	class Error {
	public:
		Error(const gchar *fmt, ...) G_GNUC_PRINTF(2, 3);
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

	static void input(gchar chr) throw (Error, ReplaceCmdline);
	State *get_next_state(gchar chr) throw (Error, ReplaceCmdline);

protected:
	static bool eval_colon(void);

	virtual State *
	custom(gchar chr) throw (Error, ReplaceCmdline)
	{
		throw SyntaxError(chr);
		return NULL;
	}
};

template <typename Type>
class MicroStateMachine {
protected:
	/* label pointers */
	typedef const void *MicroState;
	const MicroState StateStart;
#define MICROSTATE_START G_STMT_START {	\
	if (this->state != StateStart)		\
		goto *this->state;		\
} G_STMT_END

	MicroState state;

#ifdef EMSCRIPTEN
	/* FIXME: Shouldn't be required! */
	__attribute__((noinline))
#else
	inline
#endif
	void
	set(MicroState next)
	{
		if (next != state)
			undo.push_var(state) = next;
	}

public:
	MicroStateMachine() : StateStart(NULL), state(StateStart) {}
	virtual ~MicroStateMachine() {}

	virtual inline void
	reset(void)
	{
		set(StateStart);
	}

	virtual Type input(gchar chr) throw (State::Error) = 0;
};

/* avoid circular dependency on qregisters.h */
class QRegSpecMachine;

class StringBuildingMachine : public MicroStateMachine<gchar *> {
	QRegSpecMachine *qregspec_machine;

	enum Mode {
		MODE_NORMAL,
		MODE_UPPER,
		MODE_LOWER
	} mode;

	bool toctl;

public:
	StringBuildingMachine() : MicroStateMachine<gchar *>(),
				  qregspec_machine(NULL),
				  mode(MODE_NORMAL), toctl(false) {}
	~StringBuildingMachine();

	void reset(void);

	gchar *input(gchar chr) throw (State::Error);
};

/*
 * Super-class for states accepting string arguments
 * Opaquely cares about alternative-escape characters,
 * string building commands and accumulation into a string
 */
class StateExpectString : public State {
	StringBuildingMachine machine;

	gint nesting;

	bool string_building;
	bool last;

public:
	StateExpectString(bool _building = true, bool _last = true)
			 : State(), nesting(1),
			   string_building(_building), last(_last) {}

private:
	State *custom(gchar chr) throw (Error);

protected:
	virtual void initial(void) throw (Error) {}
	virtual void process(const gchar *str, gint new_chars) throw (Error) {}
	virtual State *done(const gchar *str) throw (Error) = 0;
};

class StateExpectFile : public StateExpectString {
public:
	StateExpectFile(bool _building = true, bool _last = true)
		       : StateExpectString(_building, _last) {}
};

class StateStart : public State {
public:
	StateStart();

private:
	void insert_integer(tecoInt v);
	tecoInt read_integer(void);

	tecoBool move_chars(tecoInt n);
	tecoBool move_lines(tecoInt n);

	tecoBool delete_words(tecoInt n);

	State *custom(gchar chr) throw (Error, ReplaceCmdline);
};

class StateControl : public State {
public:
	StateControl();

private:
	State *custom(gchar chr) throw (Error);
};

class StateASCII : public State {
public:
	StateASCII();

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
	extern StateASCII		ascii;
	extern StateFCommand		fcommand;
	extern StateCondCommand		condcommand;
	extern StateECommand		ecommand;
	extern StateScintilla_symbols	scintilla_symbols;
	extern StateScintilla_lParam	scintilla_lparam;
	extern StateInsert		insert;

	extern State *current;

	static inline bool
	is_string()
	{
		return dynamic_cast<StateExpectString *>(current);
	}

	static inline bool
	is_file()
	{
		return dynamic_cast<StateExpectFile *>(current);
	}
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
	void step(const gchar *macro, gint stop_pos)
		 throw (State::Error, ReplaceCmdline);
	void macro(const gchar *macro, bool locals = true)
		  throw (State::Error, ReplaceCmdline);
	bool file(const gchar *filename, bool locals = true);
}

#endif
