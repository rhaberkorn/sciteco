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
public:
	StateExpectString() : State() {}

private:
	virtual State *custom(gchar chr);

protected:
	virtual void initial(void) {}
	virtual void process(const gchar *str, gint new_chars) {}
	virtual State *done(const gchar *str) = 0;
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

#include "goto.h"
#include "qbuffers.h"

extern gint macro_pc;

extern struct States {
	StateStart	start;
	StateLabel	label;
	StateControl	control;
	StateECommand	ecommand;
	StateFile	file;
	StateInsert	insert;
} states;

extern enum Mode {
	MODE_NORMAL = 0,
	MODE_PARSE_ONLY
} mode;

#define BEGIN_EXEC(STATE) G_STMT_START {	\
	if (mode != MODE_NORMAL)		\
		return STATE;			\
} G_STMT_END

extern gchar *strings[2];

bool macro_execute(const gchar *macro);

#endif
