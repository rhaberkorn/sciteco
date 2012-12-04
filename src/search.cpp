#include <string.h>

#include <glib.h>
#include <glib/gprintf.h>

#include "sciteco.h"
#include "expressions.h"
#include "undo.h"
#include "qregisters.h"
#include "ring.h"
#include "parser.h"
#include "search.h"

namespace States {
	StateSearch			search;
	StateSearchAll			searchall;
	StateSearchKill			searchkill;
	StateSearchDelete		searchdelete;
	StateReplace			replace;
	StateReplace_insert		replace_insert;
	StateReplaceDefault		replacedefault;
	StateReplaceDefault_insert	replacedefault_insert;
}

/*
 * Command states
 */

void
StateSearch::initial(void) throw (Error)
{
	gint64 v1, v2;

	undo.push_var(parameters);

	parameters.dot = interface.ssm(SCI_GETCURRENTPOS);

	v2 = expressions.pop_num_calc();
	if (expressions.args()) {
		/* TODO: optional count argument? */
		v1 = expressions.pop_num_calc();
		if (v1 <= v2) {
			parameters.count = 1;
			parameters.from = (gint)v1;
			parameters.to = (gint)v2;
		} else {
			parameters.count = -1;
			parameters.from = (gint)v2;
			parameters.to = (gint)v1;
		}

		if (!Validate::pos(parameters.from) ||
		    !Validate::pos(parameters.to))
			throw RangeError("S");
	} else {
		parameters.count = (gint)v2;
		if (v2 >= 0) {
			parameters.from = parameters.dot;
			parameters.to = interface.ssm(SCI_GETLENGTH);
		} else {
			parameters.from = 0;
			parameters.to = parameters.dot;
		}
	}

	parameters.from_buffer = ring.current;
	parameters.to_buffer = NULL;
}

static inline const gchar *
regexp_escape_chr(gchar chr)
{
	static gchar escaped[] = {'\\', '\0', '\0'};

	escaped[1] = chr;
	return g_ascii_isalnum(chr) ? escaped + 1 : escaped;
}

gchar *
StateSearch::class2regexp(MatchState &state, const gchar *&pattern,
			  bool escape_default)
{
	while (*pattern) {
		QRegister *reg;
		gchar *temp, *temp2;

		switch (state) {
		case STATE_START:
			switch (*pattern) {
			case CTL_KEY('S'):
				return g_strdup("[:^alnum:]");
			case CTL_KEY('E'):
				state = STATE_CTL_E;
				break;
			default:
				temp = escape_default
					? g_strdup(regexp_escape_chr(*pattern))
					: NULL;
				return temp;
			}
			break;

		case STATE_CTL_E:
			switch (g_ascii_toupper(*pattern)) {
			case 'A':
				state = STATE_START;
				return g_strdup("[:alpha:]");
			/* same as <CTRL/S> */
			case 'B':
				state = STATE_START;
				return g_strdup("[:^alnum:]");
			case 'C':
				state = STATE_START;
				return g_strdup("[:alnum:].$");
			case 'D':
				state = STATE_START;
				return g_strdup("[:digit:]");
			case 'G':
				state = STATE_ANYQ;
				break;
			case 'L':
				state = STATE_START;
				return g_strdup("\r\n\v\f");
			case 'R':
				state = STATE_START;
				return g_strdup("[:alnum:]");
			case 'V':
				state = STATE_START;
				return g_strdup("[:lower:]");
			case 'W':
				state = STATE_START;
				return g_strdup("[:upper:]");
			default:
				return NULL;
			}
			break;

		case STATE_ANYQ:
			/* FIXME: Q-Register spec might get more complicated */
			reg = QRegisters::globals[g_ascii_toupper(*pattern)];
			if (!reg)
				return NULL;

			temp = reg->get_string();
			temp2 = g_regex_escape_string(temp, -1);
			g_free(temp);
			state = STATE_START;
			return temp2;

		default:
			return NULL;
		}

		pattern++;
	}

	return NULL;
}

gchar *
StateSearch::pattern2regexp(const gchar *&pattern,
			    bool single_expr)
{
	MatchState state = STATE_START;
	gchar *re = NULL;

	while (*pattern) {
		gchar *new_re, *temp;

		temp = class2regexp(state, pattern);
		if (temp) {
			new_re = g_strconcat(re ? : "", "[", temp, "]", NULL);
			g_free(temp);
			g_free(re);
			re = new_re;

			goto next;
		}
		if (!*pattern)
			break;

		switch (state) {
		case STATE_START:
			switch (*pattern) {
			case CTL_KEY('X'): String::append(re, "."); break;
			case CTL_KEY('N'): state = STATE_NOT; break;
			default:
				String::append(re, regexp_escape_chr(*pattern));
			}
			break;

		case STATE_NOT:
			state = STATE_START;
			temp = class2regexp(state, pattern, true);
			if (!temp)
				goto error;
			new_re = g_strconcat(re ? : "", "[^", temp, "]", NULL);
			g_free(temp);
			g_free(re);
			re = new_re;
			g_assert(state == STATE_START);
			break;

		case STATE_CTL_E:
			state = STATE_START;
			switch (g_ascii_toupper(*pattern)) {
			case 'M': state = STATE_MANY; break;
			case 'S': String::append(re, "\\s+"); break;
			/* same as <CTRL/X> */
			case 'X': String::append(re, "."); break;
			/* TODO: ASCII octal code!? */
			case '[':
				String::append(re, "(");
				state = STATE_ALT;
				break;
			default:
				goto error;
			}
			break;

		case STATE_MANY:
			temp = pattern2regexp(pattern, true);
			if (!temp)
				goto error;
			new_re = g_strconcat(re ? : "", "(", temp, ")+", NULL);
			g_free(temp);
			g_free(re);
			re = new_re;
			state = STATE_START;
			break;

		case STATE_ALT:
			switch (*pattern) {
			case ',':
				String::append(re, "|");
				break;
			case ']':
				String::append(re, ")");
				state = STATE_START;
				break;
			default:
				temp = pattern2regexp(pattern, true);
				if (!temp)
					goto error;
				String::append(re, temp);
				g_free(temp);
			}
			break;

		default:
			/* shouldn't happen */
			g_assert(true);
		}

next:
		if (single_expr && state == STATE_START)
			return re;

		pattern++;
	}

	if (state == STATE_ALT)
		String::append(re, ")");

	return re;

error:
	g_free(re);
	return NULL;
}

void
StateSearch::do_search(GRegex *re, gint from, gint to, gint &count)
{
	GMatchInfo *info;
	const gchar *buffer;

	gint matched_from = -1, matched_to = -1;

	buffer = (const gchar *)interface.ssm(SCI_GETCHARACTERPOINTER);
	g_regex_match_full(re, buffer, (gssize)to, from,
			   (GRegexMatchFlags)0, &info, NULL);

	if (count >= 0) {
		while (g_match_info_matches(info) && --count)
			g_match_info_next(info, NULL);

		if (!count)
			/* successful */
			g_match_info_fetch_pos(info, 0,
					       &matched_from, &matched_to);
	} else {
		/* only keep the last `count' matches, in a circular stack */
		struct Range {
			gint from, to;
		};
		Range *matched = new Range[-count];
		gint matched_total = 0, i = 0;

		while (g_match_info_matches(info)) {
			g_match_info_fetch_pos(info, 0,
					       &matched[i].from,
					       &matched[i].to);

			g_match_info_next(info, NULL);
			i = ++matched_total % -count;
		}

		count = MIN(count + matched_total, 0);
		if (!count) {
			/* successful, i points to stack bottom */
			matched_from = matched[i].from;
			matched_to = matched[i].to;
		}

		delete matched;
	}

	g_match_info_free(info);

	if (matched_from >= 0 && matched_to >= 0)
		/* match success */
		interface.ssm(SCI_SETSEL, matched_from, matched_to);
}

void
StateSearch::process(const gchar *str,
		     gint new_chars __attribute__((unused))) throw (Error)
{
	static const gint flags = G_REGEX_CASELESS | G_REGEX_MULTILINE |
				  G_REGEX_DOTALL | G_REGEX_RAW;

	QRegister *search_reg = QRegisters::globals["_"];

	gchar *re_pattern;
	GRegex *re;

	gint count = parameters.count;

	undo.push_msg(SCI_SETSEL,
		      interface.ssm(SCI_GETANCHOR),
		      interface.ssm(SCI_GETCURRENTPOS));

	search_reg->undo_set_integer();
	search_reg->set_integer(FAILURE);

	/* NOTE: pattern2regexp() modifies str pointer */
	re_pattern = pattern2regexp(str);
#ifdef DEBUG
	g_printf("REGEXP: %s\n", re_pattern);
#endif
	if (!re_pattern)
		goto failure;
	re = g_regex_new(re_pattern, (GRegexCompileFlags)flags,
			 (GRegexMatchFlags)0, NULL);
	g_free(re_pattern);
	if (!re)
		goto failure;

	if (ring.current != parameters.from_buffer) {
		ring.undo_edit();
		parameters.from_buffer->edit();
	}

	do_search(re, parameters.from, parameters.to, count);

	if (parameters.to_buffer && count) {
		Buffer *buffer = parameters.from_buffer;

		if (ring.current == buffer)
			ring.undo_edit();

		if (count > 0) {
			do {
				buffer = buffer->next() ? : ring.first();
				buffer->edit();

				if (buffer == parameters.to_buffer) {
					do_search(re, 0, parameters.dot, count);
					break;
				}

				do_search(re, 0, interface.ssm(SCI_GETLENGTH),
					  count);
			} while (count);
		} else /* count < 0 */ {
			do {
				buffer = buffer->prev() ? : ring.last();
				buffer->edit();

				if (buffer == parameters.to_buffer) {
					do_search(re, parameters.dot,
						  interface.ssm(SCI_GETLENGTH),
						  count);
					break;
				}

				do_search(re, 0, interface.ssm(SCI_GETLENGTH),
					  count);
			} while (count);
		}

		ring.current = buffer;
	}

	search_reg->set_integer(TECO_BOOL(!count));

	g_regex_unref(re);

	if (!count)
		return;

failure:
	interface.ssm(SCI_GOTOPOS, parameters.dot);
}

State *
StateSearch::done(const gchar *str) throw (Error)
{
	BEGIN_EXEC(&States::start);

	QRegister *search_reg = QRegisters::globals["_"];

	if (*str) {
		/* workaround: preserve selection (also on rubout) */
		gint anchor = interface.ssm(SCI_GETANCHOR);
		undo.push_msg(SCI_SETANCHOR, anchor);

		search_reg->undo_set_string();
		search_reg->set_string(str);

		interface.ssm(SCI_SETANCHOR, anchor);
	} else {
		gchar *search_str = search_reg->get_string();
		process(search_str, 0 /* unused */);
		g_free(search_str);
	}

	if (eval_colon())
		expressions.push(search_reg->get_integer());
	else if (IS_FAILURE(search_reg->get_integer()) &&
		 !expressions.find_op(Expressions::OP_LOOP) /* not in loop */)
		interface.msg(Interface::MSG_ERROR, "Search string not found!");

	return &States::start;
}

void
StateSearchAll::initial(void) throw (Error)
{
	gint64 v1, v2;

	undo.push_var(parameters);

	parameters.dot = interface.ssm(SCI_GETCURRENTPOS);

	v2 = expressions.pop_num_calc();
	if (expressions.args()) {
		/* TODO: optional count argument? */
		v1 = expressions.pop_num_calc();
		if (v1 <= v2) {
			parameters.count = 1;
			parameters.from_buffer = ring.find(v1);
			parameters.to_buffer = ring.find(v2);
		} else {
			parameters.count = -1;
			parameters.from_buffer = ring.find(v2);
			parameters.to_buffer = ring.find(v1);
		}

		if (!parameters.from_buffer || !parameters.to_buffer)
			throw RangeError("N");
	} else {
		parameters.count = (gint)v2;
		/* NOTE: on Q-Registers, behave like "S" */
		parameters.from_buffer = parameters.to_buffer = ring.current;
	}

	if (parameters.count >= 0) {
		parameters.from = parameters.dot;
		parameters.to = interface.ssm(SCI_GETLENGTH);
	} else {
		parameters.from = 0;
		parameters.to = parameters.dot;
	}
}

State *
StateSearchAll::done(const gchar *str) throw (Error)
{
	BEGIN_EXEC(&States::start);

	StateSearch::done(str);
	QRegisters::hook(QRegisters::HOOK_EDIT);

	return &States::start;
}

State *
StateSearchKill::done(const gchar *str) throw (Error)
{
	gint anchor;

	BEGIN_EXEC(&States::start);

	StateSearch::done(str);

	undo.push_msg(SCI_GOTOPOS, interface.ssm(SCI_GETCURRENTPOS));
	anchor = interface.ssm(SCI_GETANCHOR);
	interface.ssm(SCI_GOTOPOS, anchor);

	interface.ssm(SCI_BEGINUNDOACTION);
	interface.ssm(SCI_DELETERANGE,
		      parameters.dot, anchor - parameters.dot);
	interface.ssm(SCI_ENDUNDOACTION);
	ring.dirtify();

	undo.push_msg(SCI_UNDO);

	return &States::start;
}

State *
StateSearchDelete::done(const gchar *str) throw (Error)
{
	BEGIN_EXEC(&States::start);

	StateSearch::done(str);

	interface.ssm(SCI_BEGINUNDOACTION);
	interface.ssm(SCI_REPLACESEL, 0, (sptr_t)"");
	interface.ssm(SCI_ENDUNDOACTION);
	ring.dirtify();

	undo.push_msg(SCI_UNDO);

	return &States::start;
}

State *
StateReplace::done(const gchar *str) throw (Error)
{
	StateSearchDelete::done(str);
	return &States::replace_insert;
}

State *
StateReplaceDefault::done(const gchar *str) throw (Error)
{
	StateSearchDelete::done(str);
	return &States::replacedefault_insert;
}

State *
StateReplaceDefault_insert::done(const gchar *str) throw (Error)
{
	BEGIN_EXEC(&States::start);

	QRegister *replace_reg = QRegisters::globals["-"];

	if (*str) {
		replace_reg->undo_set_string();
		replace_reg->set_string(str);
	} else {
		gchar *replace_str = replace_reg->get_string();
		StateInsert::process(replace_str, strlen(replace_str));
		g_free(replace_str);
	}

	return &States::start;
}
