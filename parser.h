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
		InvalidQRegError(const gchar *name)
				: Error("Invalid Q-Register \"%s\"", name) {}
		InvalidQRegError(gchar name)
				: Error("Invalid Q-Register \"%c\"", name) {}
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
			STATE_CTL_EU
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

public:
	StateExpectString(bool _building = true)
			 : State(), nesting(1), string_building(_building) {}

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

class StateFlowCommand : public State {
public:
	StateFlowCommand();

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

class StateScintilla : public StateExpectString {
private:
	State *done(const gchar *str) throw (Error);
};

class StateInsert : public StateExpectString {
private:
	void initial(void) throw (Error);
	void process(const gchar *str, gint new_chars) throw (Error);
	State *done(const gchar *str) throw (Error);
};

class StateSearch : public StateExpectString {
private:
	struct Parameters {
		gint dot;
		gint from, to;
		gint count;
	} parameters;

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

	void initial(void) throw (Error);
	void process(const gchar *str, gint new_chars) throw (Error);
	State *done(const gchar *str) throw (Error);
};

extern gint macro_pc;

namespace States {
	extern StateStart 	start;
	extern StateControl	control;
	extern StateFlowCommand	flowcommand;
	extern StateCondCommand	condcommand;
	extern StateECommand	ecommand;
	extern StateScintilla	scintilla;
	extern StateInsert	insert;
	extern StateSearch	search;

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

extern gchar *strings[2];
extern gchar escape_char;

void macro_execute(const gchar *macro) throw (State::Error);
bool file_execute(const gchar *filename);

#endif
