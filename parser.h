#ifndef __PARSER_H
#define __PARSER_H

#include <glib.h>
#include <glib/gprintf.h>

#include "sciteco.h"

class State {
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

	static bool input(gchar chr);
	State *get_next_state(gchar chr);

protected:
	static bool eval_colon(void);

	virtual State *
	custom(gchar chr)
	{
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

public:
	StateExpectString() : State() {}

private:
	gchar *machine_input(gchar key);
	State *custom(gchar chr);

protected:
	virtual void initial(void) {}
	virtual void process(const gchar *str, gint new_chars) {}
	virtual State *done(const gchar *str) = 0;
};

class QRegister;

/*
 * Super class for states accepting Q-Register specifications
 */
class StateExpectQReg : public State {
public:
	StateExpectQReg();

private:
	State *custom(gchar chr);

protected:
	/*
	 * FIXME: would be nice to pass reg as reference, but there are
	 * circular header dependencies...
	 */
	virtual State *got_register(QRegister *reg) = 0;
};

class StateStart : public State {
public:
	StateStart();

private:
	void move(gint64 n);
	void move_lines(gint64 n);

	State *custom(gchar chr);
};

class StateControl : public State {
public:
	StateControl();

private:
	State *custom(gchar chr);
};

class StateFlowCommand : public State {
public:
	StateFlowCommand();

private:
	State *custom(gchar chr);
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

class StateInsert : public StateExpectString {
private:
	void initial(void);
	void process(const gchar *str, gint new_chars);
	State *done(const gchar *str);
};

class StateSearch : public StateExpectString {
private:
	struct Parameters {
		gint dot;
		gint from, to;
		gint count;
	} parameters;

	void initial(void);
	void process(const gchar *str, gint new_chars);
	State *done(const gchar *str);
};

extern gint macro_pc;

namespace States {
	extern StateStart 	start;
	extern StateControl	control;
	extern StateFlowCommand	flowcommand;
	extern StateCondCommand	condcommand;
	extern StateECommand	ecommand;
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

bool macro_execute(const gchar *macro);
bool file_execute(const gchar *filename);

#endif
