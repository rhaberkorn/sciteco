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

#ifndef __PARSER_H
#define __PARSER_H

#include <string.h>

#include <glib.h>

#include "sciteco.h"
#include "memory.h"
#include "undo.h"
#include "error.h"
#include "expressions.h"

namespace SciTECO {

/* TECO uses only lower 7 bits for commands */
#define MAX_TRANSITIONS	127

class State : public Object {
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

	static void input(gchar chr);
	State *get_next_state(gchar chr);

	/**
	 * Provide interactive feedback.
	 *
	 * This gets called whenever a state with
	 * immediate interactive feedback should provide that
	 * feedback; allowing them to optimize batch mode,
	 * macro and many other cases.
	 */
	virtual void refresh(void) {}

	/**
	 * Called at the end of a macro.
	 * Most states/commands are not allowed to end unterminated
	 * at the end of a macro.
	 */
	virtual void
	end_of_macro(void)
	{
		throw Error("Unterminated command");
	}

protected:
	static bool eval_colon(void);

	/** Get next state given an input character */
	virtual State *
	custom(gchar chr)
	{
		throw SyntaxError(chr);
		return NULL;
	}
};

template <typename Type>
class MicroStateMachine : public Object {
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

	virtual bool input(gchar chr, Type &result) = 0;
};

/* avoid circular dependency on qregisters.h */
class QRegSpecMachine;

class StringBuildingMachine : public MicroStateMachine<gchar *> {
	enum Mode {
		MODE_NORMAL,
		MODE_UPPER,
		MODE_LOWER
	} mode;

	bool toctl;

public:
	QRegSpecMachine *qregspec_machine;

	StringBuildingMachine() : MicroStateMachine<gchar *>(),
				  mode(MODE_NORMAL), toctl(false),
				  qregspec_machine(NULL) {}
	~StringBuildingMachine();

	void reset(void);

	bool input(gchar chr, gchar *&result);
};

/*
 * Super-class for states accepting string arguments
 * Opaquely cares about alternative-escape characters,
 * string building commands and accumulation into a string
 */
class StateExpectString : public State {
	gsize insert_len;

	gint nesting;

	bool string_building;
	bool last;

public:
	/*
	 * FIXME: Only public as long as cmdline.cpp
	 * needs to access it.
	 * Can be avoided one process_edit_cmd() is in State.
	 */
	StringBuildingMachine machine;

	StateExpectString(bool _building = true, bool _last = true)
			 : insert_len(0), nesting(1),
			   string_building(_building), last(_last) {}

private:
	State *custom(gchar chr);
	void refresh(void);

protected:
	virtual void initial(void) {}
	virtual void process(const gchar *str, gint new_chars) {}
	virtual State *done(const gchar *str) = 0;
};

class StateExpectFile : public StateExpectString {
public:
	StateExpectFile(bool _building = true, bool _last = true)
		       : StateExpectString(_building, _last) {}

private:
	State *done(const gchar *str);

protected:
	virtual State *got_file(const gchar *filename) = 0;
};

class StateExpectDir : public StateExpectFile {
public:
	StateExpectDir(bool _building = true, bool _last = true)
		      : StateExpectFile(_building, _last) {}
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

	State *custom(gchar chr);

	void end_of_macro(void) {}
};

class StateControl : public State {
public:
	StateControl();

private:
	State *custom(gchar chr);
};

class StateASCII : public State {
public:
	StateASCII();

private:
	State *custom(gchar chr);
};

class StateEscape : public State {
public:
	StateEscape();

private:
	State *custom(gchar chr);

	void end_of_macro(void);
};

class StateFCommand : public State {
public:
	StateFCommand();

private:
	State *custom(gchar chr);
};

class UndoTokenChangeDir : public UndoToken {
	gchar *dir;

public:
	/**
	 * Construct undo token.
	 *
	 * This passes ownership of the directory string
	 * to the undo token object.
	 */
	UndoTokenChangeDir(gchar *_dir) : dir(_dir) {}
	~UndoTokenChangeDir()
	{
		g_free(dir);
	}

	void run(void);
};

class StateChangeDir : public StateExpectDir {
private:
	State *got_file(const gchar *filename);
};

class StateCondCommand : public State {
public:
	StateCondCommand();

private:
	State *custom(gchar chr);
};

class StateECommand : public State {
public:
	StateECommand();

private:
	State *custom(gchar chr);
};

class StateScintilla_symbols : public StateExpectString {
public:
	StateScintilla_symbols() : StateExpectString(true, false) {}

private:
	State *done(const gchar *str);
};

class StateScintilla_lParam : public StateExpectString {
private:
	State *done(const gchar *str);
};

/*
 * also serves as base class for replace-insertion states
 */
class StateInsert : public StateExpectString {
public:
	StateInsert(bool building = true)
	           : StateExpectString(building) {}

protected:
	void initial(void);
	void process(const gchar *str, gint new_chars);
	State *done(const gchar *str);
};

class StateInsertIndent : public StateInsert {
protected:
	void initial(void);
};

namespace States {
	extern StateStart		start;
	extern StateControl		control;
	extern StateASCII		ascii;
	extern StateEscape		escape;
	extern StateFCommand		fcommand;
	extern StateChangeDir		changedir;
	extern StateCondCommand		condcommand;
	extern StateECommand		ecommand;
	extern StateScintilla_symbols	scintilla_symbols;
	extern StateScintilla_lParam	scintilla_lparam;
	extern StateInsert		insert_building;
	extern StateInsert		insert_nobuilding;
	extern StateInsertIndent	insert_indent;

	extern State *current;

	static inline bool
	is_start()
	{
		/*
		 * The "escape" state exists only as a hack,
		 * to support $$. Otherwise it should behave
		 * like the start state.
		 */
		return current == &start || current == &escape;
	}

	static inline bool
	is_string()
	{
		return dynamic_cast<StateExpectString *>(current);
	}

	static inline bool
	is_insertion()
	{
		return dynamic_cast<StateInsert *>(current);
	}

	static inline bool
	is_file()
	{
		return dynamic_cast<StateExpectFile *>(current);
	}

	static inline bool
	is_dir()
	{
		return dynamic_cast<StateExpectDir *>(current);
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

struct LoopContext {
	/** how many iterations are left */
	tecoInt counter;
	/** Program counter of loop start command */
	guint pc : sizeof(guint)*8 - 1;
	/**
	 * Whether the loop represents an argument
	 * barrier or not (it "passes through"
	 * stack arguments).
	 *
	 * Since the program counter is usually
	 * a signed integer, it's ok steal one
	 * bit for the pass_through flag.
	 */
	bool pass_through : 1;
};
typedef ValueStack<LoopContext> LoopStack;
extern LoopStack loop_stack;

namespace Execute {
	void step(const gchar *macro, gint stop_pos);
	void macro(const gchar *macro, bool locals = true);
	void file(const gchar *filename, bool locals = true);
}

} /* namespace SciTECO */

#endif
