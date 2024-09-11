/*
 * Copyright (C) 2012-2024 Robin Haberkorn
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include <glib.h>
#include <glib/gprintf.h>

#include "sciteco.h"
#include "string-utils.h"
#include "expressions.h"
#include "interface.h"
#include "memory.h"
#include "undo.h"
#include "qreg.h"
#include "ring.h"
#include "parser.h"
#include "core-commands.h"
#include "error.h"
#include "search.h"

typedef struct {
	gssize dot;
	gssize from, to;
	gint count;

	teco_buffer_t *from_buffer, *to_buffer;
} teco_search_parameters_t;

TECO_DEFINE_UNDO_OBJECT_OWN(parameters, teco_search_parameters_t, /* don't delete */);

/*
 * FIXME: Global state should be part of teco_machine_main_t
 */
static teco_search_parameters_t teco_search_parameters;

static teco_machine_qregspec_t *teco_search_qreg_machine = NULL;

static gboolean
teco_state_search_initial(teco_machine_main_t *ctx, GError **error)
{
	if (ctx->mode > TECO_MODE_NORMAL)
		return TRUE;

	teco_undo_guint(ctx->expectstring.machine.codepage) = teco_interface_get_codepage();

	if (G_UNLIKELY(!teco_search_qreg_machine))
		teco_search_qreg_machine = teco_machine_qregspec_new(TECO_QREG_REQUIRED, ctx->qreg_table_locals,
		                                                     ctx->parent.must_undo);

	teco_undo_object_parameters_push(&teco_search_parameters);
	teco_search_parameters.dot = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);

	teco_int_t v1, v2;
	if (!teco_expressions_pop_num_calc(&v2, teco_num_sign, error))
		return FALSE;
	if (teco_expressions_args()) {
		/* TODO: optional count argument? */
		if (!teco_expressions_pop_num_calc(&v1, 0, error))
			return FALSE;
		if (v1 <= v2) {
			teco_search_parameters.count = 1;
			teco_search_parameters.from = teco_interface_glyphs2bytes(v1);
			teco_search_parameters.to = teco_interface_glyphs2bytes(v2);
		} else {
			teco_search_parameters.count = -1;
			teco_search_parameters.from = teco_interface_glyphs2bytes(v2);
			teco_search_parameters.to = teco_interface_glyphs2bytes(v1);
		}

		if (teco_search_parameters.from < 0 ||
		    teco_search_parameters.to < 0) {
			/*
			 * FIXME: In derived classes, the command name will
			 * no longer be correct.
			 * Better use a generic error message and prefix the error in the
			 * derived states.
			 */
			teco_error_range_set(error, "S");
			return FALSE;
		}
	} else {
		teco_search_parameters.count = (gint)v2;
		if (v2 >= 0) {
			teco_search_parameters.from = teco_search_parameters.dot;
			teco_search_parameters.to = teco_interface_ssm(SCI_GETLENGTH, 0, 0);
		} else {
			teco_search_parameters.from = 0;
			teco_search_parameters.to = teco_search_parameters.dot;
		}
	}

	teco_search_parameters.from_buffer = teco_qreg_current ? NULL : teco_ring_current;
	teco_search_parameters.to_buffer = NULL;
	return TRUE;
}

typedef enum {
	TECO_SEARCH_STATE_START,
	TECO_SEARCH_STATE_NOT,
	TECO_SEARCH_STATE_CTL_E,
	TECO_SEARCH_STATE_ANYQ,
	TECO_SEARCH_STATE_MANY,
	TECO_SEARCH_STATE_ALT
} teco_search_state_t;

/**
 * Convert a SciTECO pattern character class to a regular
 * expression character class.
 * It will throw an error for wrong class specs (like invalid
 * Q-Registers) but not incomplete ones.
 *
 * @param state Initial pattern converter state.
 *              May be modified on return, e.g. when ^E has been
 *              scanned without a valid class.
 * @param pattern The character class definition to convert.
 *                This may point into a longer pattern.
 *                The pointer is modified and always left after
 *                the last character used, so it may point to the
 *                terminating null byte after the call.
 * @param escape_default Whether to treat single characters
 *                       as classes or not.
 * @param error A GError.
 * @return A regular expression string or NULL in case of error.
 *         An empty string signals an incomplete class specification.
 *         When a non-empty string is returned, the state has always
 *         been reset to TECO_STATE_STATE_START.
 *         Must be freed with g_free().
 *
 * @fixme The allocations could be avoided by letting it append
 * to the target regexp teco_string_t directly.
 */
static gchar *
teco_class2regexp(teco_search_state_t *state, teco_string_t *pattern,
                  gboolean escape_default, GError **error)
{
	while (pattern->len > 0) {
		switch (*state) {
		case TECO_SEARCH_STATE_START:
			switch (*pattern->data) {
			case TECO_CTL_KEY('S'):
				pattern->data++;
				pattern->len--;
				return g_strdup("[:^alnum:]");
			case TECO_CTL_KEY('E'):
				*state = TECO_SEARCH_STATE_CTL_E;
				break;
			default:
				/*
				 * Either a single character "class" or
				 * not a valid class at all.
				 */
				if (!escape_default)
					return g_strdup("");
				gsize len = g_utf8_next_char(pattern->data) - pattern->data;
				gchar *escaped = g_regex_escape_string(pattern->data, len);
				pattern->data += len;
				pattern->len -= len;
				return escaped;
			}
			break;

		case TECO_SEARCH_STATE_CTL_E:
			switch (teco_ascii_toupper(*pattern->data)) {
			case 'A':
				pattern->data++;
				pattern->len--;
				*state = TECO_SEARCH_STATE_START;
				return g_strdup("[:alpha:]");
			/* same as <CTRL/S> */
			case 'B':
				pattern->data++;
				pattern->len--;
				*state = TECO_SEARCH_STATE_START;
				return g_strdup("[:^alnum:]");
			case 'C':
				pattern->data++;
				pattern->len--;
				*state = TECO_SEARCH_STATE_START;
				return g_strdup("[:alnum:].$");
			case 'D':
				pattern->data++;
				pattern->len--;
				*state = TECO_SEARCH_STATE_START;
				return g_strdup("[:digit:]");
			case 'G':
				*state = TECO_SEARCH_STATE_ANYQ;
				break;
			case 'L':
				pattern->data++;
				pattern->len--;
				*state = TECO_SEARCH_STATE_START;
				return g_strdup("\r\n\v\f");
			case 'R':
				pattern->data++;
				pattern->len--;
				*state = TECO_SEARCH_STATE_START;
				return g_strdup("[:alnum:]");
			case 'V':
				pattern->data++;
				pattern->len--;
				*state = TECO_SEARCH_STATE_START;
				return g_strdup("[:lower:]");
			case 'W':
				pattern->data++;
				pattern->len--;
				*state = TECO_SEARCH_STATE_START;
				return g_strdup("[:upper:]");
			default:
				/*
				 * Not a valid ^E class, but could still
				 * be a higher-level ^E pattern.
				 */
				return g_strdup("");
			}
			break;

		case TECO_SEARCH_STATE_ANYQ: {
			teco_qreg_t *reg;

			/* FIXME: Once the parser is UTF-8, we need pass a code point here */
			switch (teco_machine_qregspec_input(teco_search_qreg_machine,
			                                    *pattern->data, &reg, NULL, error)) {
			case TECO_MACHINE_QREGSPEC_ERROR:
				return NULL;

			case TECO_MACHINE_QREGSPEC_MORE:
				/* incomplete, but consume byte */
				break;

			case TECO_MACHINE_QREGSPEC_DONE:
				teco_machine_qregspec_reset(teco_search_qreg_machine);

				g_auto(teco_string_t) str = {NULL, 0};
				if (!reg->vtable->get_string(reg, &str.data, &str.len, NULL, error))
					return NULL;

				pattern->data++;
				pattern->len--;
				*state = TECO_SEARCH_STATE_START;
				return g_regex_escape_string(str.data, str.len);
			}
			break;
		}

		default:
			/*
			 * Not a class, but could still be any other
			 * high-level pattern.
			 */
			return g_strdup("");
		}

		pattern->data++;
		pattern->len--;
	}

	/*
	 * End of string. May happen for empty strings but also
	 * incomplete ^E or ^EG classes.
	 */
	return g_strdup("");
}

/**
 * Convert SciTECO pattern to regular expression.
 * It will throw an error for definitely wrong pattern constructs
 * but not for incomplete patterns (a necessity of search-as-you-type).
 *
 * @bug Incomplete patterns after a pattern has been closed (i.e. its
 * string argument) are currently not reported as errors.
 *
 * @param pattern The pattern to scan through.
 *                It must always be in UTF-8.
 *                Modifies the pointer to point after the last
 *                successfully scanned character, so it can be
 *                called recursively. It may also point to the
 *                terminating null byte after the call.
 * @param single_expr Whether to scan a single pattern
 *                    expression or an arbitrary sequence.
 * @param error A GError.
 * @return The regular expression string or NULL in case of GError.
 *         Must be freed with g_free().
 */
static gchar *
teco_pattern2regexp(teco_string_t *pattern, gboolean single_expr, GError **error)
{
	teco_search_state_t state = TECO_SEARCH_STATE_START;
	g_auto(teco_string_t) re = {NULL, 0};

	do {
		/*
		 * First check whether it is a class.
		 * This will not treat individual characters
		 * as classes, so we do not convert them to regexp
		 * classes unnecessarily.
		 */
		g_autofree gchar *temp = teco_class2regexp(&state, pattern, FALSE, error);
		if (!temp)
			return NULL;

		if (*temp) {
			g_assert(state == TECO_SEARCH_STATE_START);

			teco_string_append_c(&re, '[');
			teco_string_append(&re, temp, strlen(temp));
			teco_string_append_c(&re, ']');

			/* teco_class2regexp() already consumed all the bytes */
			continue;
		}

		if (!pattern->len)
			/* end of pattern */
			break;

		switch (state) {
		case TECO_SEARCH_STATE_START:
			switch (*pattern->data) {
			case TECO_CTL_KEY('X'): teco_string_append_c(&re, '.'); break;
			case TECO_CTL_KEY('N'): state = TECO_SEARCH_STATE_NOT; break;
			default: {
				gsize len = g_utf8_next_char(pattern->data) - pattern->data;
				/* the allocation could theoretically be avoided by escaping char-wise */
				g_autofree gchar *escaped = g_regex_escape_string(pattern->data, len);
				teco_string_append(&re, escaped, strlen(escaped));
				pattern->data += len;
				pattern->len -= len;
				continue;
			}
			}
			break;

		case TECO_SEARCH_STATE_NOT: {
			state = TECO_SEARCH_STATE_START;
			g_autofree gchar *temp = teco_class2regexp(&state, pattern, TRUE, error);
			if (!temp)
				return NULL;
			if (!*temp)
				/* a complete class is strictly required */
				return g_strdup("");
			g_assert(state == TECO_SEARCH_STATE_START);

			teco_string_append(&re, "[^", 2);
			teco_string_append(&re, temp, strlen(temp));
			teco_string_append(&re, "]", 1);

			/* class2regexp() already consumed all the bytes */
			continue;
		}

		case TECO_SEARCH_STATE_CTL_E:
			state = TECO_SEARCH_STATE_START;
			switch (teco_ascii_toupper(*pattern->data)) {
			case 'M': state = TECO_SEARCH_STATE_MANY; break;
			case 'S': teco_string_append(&re, "\\s+", 3); break;
			/* same as <CTRL/X> */
			case 'X': teco_string_append_c(&re, '.'); break;
			/* TODO: ASCII octal code!? */
			case '[':
				teco_string_append_c(&re, '(');
				state = TECO_SEARCH_STATE_ALT;
				break;
			default:
				teco_error_syntax_set(error, *pattern->data);
				return NULL;
			}
			break;

		case TECO_SEARCH_STATE_MANY: {
			/* consume exactly one pattern element */
			g_autofree gchar *temp = teco_pattern2regexp(pattern, TRUE, error);
			if (!temp)
				return NULL;
			if (!*temp)
				/* a complete expression is strictly required */
				return g_strdup("");

			teco_string_append(&re, "(", 1);
			teco_string_append(&re, temp, strlen(temp));
			teco_string_append(&re, ")+", 2);
			state = TECO_SEARCH_STATE_START;

			/* teco_pattern2regexp() already consumed all the bytes */
			continue;
		}

		case TECO_SEARCH_STATE_ALT:
			switch (*pattern->data) {
			case ',':
				teco_string_append_c(&re, '|');
				break;
			case ']':
				teco_string_append_c(&re, ')');
				state = TECO_SEARCH_STATE_START;
				break;
			default: {
				g_autofree gchar *temp = teco_pattern2regexp(pattern, TRUE, error);
				if (!temp)
					return NULL;
				if (!*temp)
					/* a complete expression is strictly required */
					return g_strdup("");

				teco_string_append(&re, temp, strlen(temp));

				/* pattern2regexp() already consumed all the bytes */
				continue;
			}
			}
			break;

		default:
			/* shouldn't happen */
			g_assert_not_reached();
		}

		pattern->data++;
		pattern->len--;
	} while (!single_expr || state != TECO_SEARCH_STATE_START);

	/*
	 * Complete open alternative.
	 * This could be handled like an incomplete expression,
	 * but closing it automatically improved search-as-you-type.
	 */
	if (state == TECO_SEARCH_STATE_ALT)
		teco_string_append_c(&re, ')');

	g_assert(!teco_string_contains(&re, '\0'));
	return g_steal_pointer(&re.data) ? : g_strdup("");
}

static gboolean
teco_do_search(GRegex *re, gint from, gint to, gint *count, GError **error)
{
	g_autoptr(GMatchInfo) info = NULL;
	const gchar *buffer = (const gchar *)teco_interface_ssm(SCI_GETCHARACTERPOINTER, 0, 0);
	GError *tmp_error = NULL;

	/*
	 * NOTE: The return boolean does NOT signal whether an error was generated.
	 */
	g_regex_match_full(re, buffer, (gssize)to, from, 0, &info, &tmp_error);
	if (tmp_error) {
		g_propagate_error(error, tmp_error);
		return FALSE;
	}

	gint matched_from = -1, matched_to = -1;

	if (*count >= 0) {
		while (g_match_info_matches(info) && --(*count)) {
			/*
			 * NOTE: The return boolean does NOT signal whether an error was generated.
			 */
			g_match_info_next(info, &tmp_error);
			if (tmp_error) {
				g_propagate_error(error, tmp_error);
				return FALSE;
			}
		}

		if (!*count)
			/* successful */
			g_match_info_fetch_pos(info, 0,
					       &matched_from, &matched_to);
	} else {
		/* only keep the last `count' matches, in a circular stack */
		typedef struct {
			gint from, to;
		} teco_range_t;

		gsize matched_size = sizeof(teco_range_t) * -*count;

		/*
		 * matched_size could overflow.
		 * NOTE: Glib 2.48 has g_size_checked_mul() which uses
		 * compiler intrinsics.
		 */
		if (matched_size / sizeof(teco_range_t) != -*count)
			/* guaranteed to fail either teco_memory_check() or g_malloc() */
			matched_size = G_MAXSIZE;

		/*
		 * NOTE: It's theoretically possible that the allocation of the `matched`
		 * array causes an OOM if (-count) is large enough and regular
		 * memory limiting in teco_machine_main_step() wouldn't help.
		 * That's why we exceptionally have to check before allocating.
		 */
		if (!teco_memory_check(matched_size, error))
			return FALSE;

		g_autofree teco_range_t *matched = g_malloc(matched_size);

		gint matched_total = 0, i = 0;

		while (g_match_info_matches(info)) {
			g_match_info_fetch_pos(info, 0,
					       &matched[i].from, &matched[i].to);

			/*
			 * NOTE: The return boolean does NOT signal whether an error was generated.
			 */
			g_match_info_next(info, &tmp_error);
			if (tmp_error) {
				g_propagate_error(error, tmp_error);
				return FALSE;
			}

			i = ++matched_total % -(*count);
		}

		*count = MIN(*count + matched_total, 0);
		if (!*count) {
			/* successful -> i points to stack bottom */
			matched_from = matched[i].from;
			matched_to = matched[i].to;
		}
	}

	if (matched_from >= 0 && matched_to >= 0)
		/* match success */
		teco_interface_ssm(SCI_SETSEL, matched_from, matched_to);

	return TRUE;
}

static gboolean
teco_state_search_process(teco_machine_main_t *ctx, const teco_string_t *str, gsize new_chars, GError **error)
{
	/* FIXME: Should G_REGEX_OPTIMIZE be added under certain circumstances? */
	GRegexCompileFlags flags = G_REGEX_CASELESS | G_REGEX_MULTILINE | G_REGEX_DOTALL;

	/* this is set in teco_state_search_initial() */
	if (ctx->expectstring.machine.codepage != SC_CP_UTF8)
		/* single byte encoding */
		flags |= G_REGEX_RAW;

	if (teco_current_doc_must_undo())
		undo__teco_interface_ssm(SCI_SETSEL,
		                         teco_interface_ssm(SCI_GETANCHOR, 0, 0),
		                         teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0));

	teco_qreg_t *search_reg = teco_qreg_table_find(&teco_qreg_table_globals, "_", 1);
	g_assert(search_reg != NULL);
	if (!search_reg->vtable->undo_set_integer(search_reg, error) ||
	    !search_reg->vtable->set_integer(search_reg, TECO_FAILURE, error))
		return FALSE;

	g_autoptr(GRegex) re = NULL;
	teco_string_t pattern = *str;
	/* NOTE: teco_pattern2regexp() modifies str pointer */
	g_autofree gchar *re_pattern = teco_pattern2regexp(&pattern, FALSE, error);
	if (!re_pattern)
		return FALSE;
	teco_machine_qregspec_reset(teco_search_qreg_machine);
#ifdef DEBUG
	g_printf("REGEXP: %s\n", re_pattern);
#endif
	if (!*re_pattern)
		goto failure;
	/*
	 * FIXME: Should we propagate at least some of the errors?
	 */
	re = g_regex_new(re_pattern, flags, 0, NULL);
	if (!re)
		goto failure;

	if (!teco_qreg_current &&
	    teco_ring_current != teco_search_parameters.from_buffer) {
		teco_ring_undo_edit();
		teco_buffer_edit(teco_search_parameters.from_buffer);
	}

	gint count = teco_search_parameters.count;

	if (!teco_do_search(re, teco_search_parameters.from, teco_search_parameters.to, &count, error))
		return FALSE;

	if (teco_search_parameters.to_buffer && count) {
		teco_buffer_t *buffer = teco_search_parameters.from_buffer;

		if (teco_ring_current == buffer)
			teco_ring_undo_edit();

		if (count > 0) {
			do {
				buffer = teco_buffer_next(buffer) ? : teco_ring_first();
				teco_buffer_edit(buffer);

				if (buffer == teco_search_parameters.to_buffer) {
					if (!teco_do_search(re, 0, teco_search_parameters.dot, &count, error))
						return FALSE;
					break;
				}

				if (!teco_do_search(re, 0, teco_interface_ssm(SCI_GETLENGTH, 0, 0),
				                    &count, error))
					return FALSE;
			} while (count);
		} else /* count < 0 */ {
			do {
				buffer = teco_buffer_prev(buffer) ? : teco_ring_last();
				teco_buffer_edit(buffer);

				if (buffer == teco_search_parameters.to_buffer) {
					if (!teco_do_search(re, teco_search_parameters.dot,
					                    teco_interface_ssm(SCI_GETLENGTH, 0, 0),
					                    &count, error))
						return FALSE;
					break;
				}

				if (!teco_do_search(re, 0, teco_interface_ssm(SCI_GETLENGTH, 0, 0),
				                    &count, error))
					return FALSE;
			} while (count);
		}

		/*
		 * FIXME: Why is this necessary?
		 */
		teco_ring_current = buffer;
	}

	if (!search_reg->vtable->set_integer(search_reg, teco_bool(!count), error))
		return FALSE;

	if (!count)
		return TRUE;

failure:
	teco_interface_ssm(SCI_GOTOPOS, teco_search_parameters.dot, 0);
	return TRUE;
}

static teco_state_t *
teco_state_search_done(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	if (ctx->mode > TECO_MODE_NORMAL)
		return &teco_state_start;

	teco_qreg_t *search_reg = teco_qreg_table_find(&teco_qreg_table_globals, "_", 1);
	g_assert(search_reg != NULL);

	if (str->len > 0) {
		/* workaround: preserve selection (also on rubout) */
		gint anchor = teco_interface_ssm(SCI_GETANCHOR, 0, 0);
		if (teco_current_doc_must_undo())
			undo__teco_interface_ssm(SCI_SETANCHOR, anchor, 0);

		if (!search_reg->vtable->undo_set_string(search_reg, error) ||
		    !search_reg->vtable->set_string(search_reg, str->data, str->len,
		                                    teco_default_codepage(), error))
			return NULL;

		teco_interface_ssm(SCI_SETANCHOR, anchor, 0);
	} else {
		g_auto(teco_string_t) search_str = {NULL, 0};
		if (!search_reg->vtable->get_string(search_reg, &search_str.data, &search_str.len,
		                                    NULL, error) ||
		    !teco_state_search_process(ctx, &search_str, search_str.len, error))
			return NULL;
	}

	teco_int_t search_state;
	if (!search_reg->vtable->get_integer(search_reg, &search_state, error))
		return FALSE;

	if (teco_machine_main_eval_colon(ctx))
		teco_expressions_push(search_state);
	else if (teco_is_failure(search_state) &&
	         !teco_loop_stack->len /* not in loop */)
		teco_interface_msg(TECO_MSG_ERROR, "Search string not found!");

	return &teco_state_start;
}

/**
 * @class TECO_DEFINE_STATE_SEARCH
 * @implements TECO_DEFINE_STATE_EXPECTSTRING
 * @ingroup states
 *
 * @fixme We need to provide a process_edit_cmd_cb since search patterns
 * can also contain Q-Register references.
 */
#define TECO_DEFINE_STATE_SEARCH(NAME, ...) \
	TECO_DEFINE_STATE_EXPECTSTRING(NAME, \
		.initial_cb = (teco_state_initial_cb_t)teco_state_search_initial, \
		.expectstring.process_cb = teco_state_search_process, \
		.expectstring.done_cb = NAME##_done, \
		##__VA_ARGS__ \
	)

/*$ S search pattern
 * S[pattern]$ -- Search for pattern
 * [n]S[pattern]$
 * -S[pattern]$
 * from,toS[pattern]$
 * :S[pattern]$ -> Success|Failure
 * [n]:S[pattern]$ -> Success|Failure
 * -:S[pattern]$ -> Success|Failure
 * from,to:S[pattern]$ -> Success|Failure
 *
 * Search for <pattern> in the current buffer/Q-Register.
 * Search order and range depends on the arguments given.
 * By default without any arguments, S will search forward
 * from dot till file end.
 * The optional single argument specifies the occurrence
 * to search (1 is the first occurrence, 2 the second, etc.).
 * Negative values for <n> perform backward searches.
 * If missing, the sign prefix is implied for <n>.
 * Therefore \(lq-S\(rq will search for the first occurrence
 * of <pattern> before dot.
 *
 * If two arguments are specified on the command,
 * search will be bounded in the character range <from> up to
 * <to>, and only the first occurrence will be searched.
 * <from> might be larger than <to> in which case a backward
 * search is performed in the selected range.
 *
 * After performing the search, the search <pattern> is saved
 * in the global search Q-Register \(lq_\(rq.
 * A success/failure condition boolean is saved in that
 * register's integer part.
 * <pattern> may be omitted in which case the pattern of
 * the last search or search and replace command will be
 * implied by using the contents of register \(lq_\(rq
 * (this could of course also be manually set).
 *
 * After a successful search, the pointer is positioned after
 * the found text in the buffer.
 * An unsuccessful search will display an error message but
 * not actually yield an error.
 * The message displaying is suppressed when executed from loops
 * and register \(lq_\(rq is the implied argument for break-commands
 * so that a search-break idiom can be implemented as follows:
 * .EX
 * <Sfoo$; ...>
 * .EE
 * Alternatively, S may be colon-modified in which case it returns
 * a condition boolean that may be directly evaluated by a
 * conditional or break-command.
 *
 * In interactive mode, searching will be performed immediately
 * (\(lqsearch as you type\(rq) highlighting matched text
 * on the fly.
 * Changing the <pattern> results in the search being reperformed
 * from the beginning.
 */
TECO_DEFINE_STATE_SEARCH(teco_state_search);

static gboolean
teco_state_search_all_initial(teco_machine_main_t *ctx, GError **error)
{
	if (ctx->mode > TECO_MODE_NORMAL)
		return TRUE;

	teco_undo_object_parameters_push(&teco_search_parameters);
	teco_search_parameters.dot = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);

	teco_int_t v1, v2;

	if (!teco_expressions_pop_num_calc(&v2, teco_num_sign, error))
		return FALSE;
	if (teco_expressions_args()) {
		/* TODO: optional count argument? */
		if (!teco_expressions_pop_num_calc(&v1, 0, error))
			return FALSE;
		if (v1 <= v2) {
			teco_search_parameters.count = 1;
			teco_search_parameters.from_buffer = teco_ring_find(v1);
			teco_search_parameters.to_buffer = teco_ring_find(v2);
		} else {
			teco_search_parameters.count = -1;
			teco_search_parameters.from_buffer = teco_ring_find(v2);
			teco_search_parameters.to_buffer = teco_ring_find(v1);
		}

		if (!teco_search_parameters.from_buffer || !teco_search_parameters.to_buffer) {
			teco_error_range_set(error, "N");
			return FALSE;
		}
	} else {
		teco_search_parameters.count = (gint)v2;
		/* NOTE: on Q-Registers, behave like "S" */
		if (teco_qreg_current) {
			teco_search_parameters.from_buffer = NULL;
			teco_search_parameters.to_buffer = NULL;
		} else {
			teco_search_parameters.from_buffer = teco_ring_current;
			teco_search_parameters.to_buffer = teco_ring_current;
		}
	}

	if (teco_search_parameters.count >= 0) {
		teco_search_parameters.from = teco_search_parameters.dot;
		teco_search_parameters.to = teco_interface_ssm(SCI_GETLENGTH, 0, 0);
	} else {
		teco_search_parameters.from = 0;
		teco_search_parameters.to = teco_search_parameters.dot;
	}

	return TRUE;
}

static teco_state_t *
teco_state_search_all_done(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	if (ctx->mode <= TECO_MODE_NORMAL &&
	    (!teco_state_search_done(ctx, str, error) ||
	     !teco_ed_hook(TECO_ED_HOOK_EDIT, error)))
		return NULL;

	return &teco_state_start;
}

/*$ N
 * [n]N[pattern]$ -- Search over buffer-boundaries
 * -N[pattern]$
 * from,toN[pattern]$
 * [n]:N[pattern]$ -> Success|Failure
 * -:N[pattern]$ -> Success|Failure
 * from,to:N[pattern]$ -> Success|Failure
 *
 * Search for <pattern> over buffer boundaries.
 * This command is similar to the regular search command
 * (S) but will continue to search for occurrences of
 * pattern when the end or beginning of the current buffer
 * is reached.
 * Occurrences of <pattern> spanning over buffer boundaries
 * will not be found.
 * When searching forward N will start in the current buffer
 * at dot, continue with the next buffer in the ring searching
 * the entire buffer until it reaches the end of the buffer
 * ring, continue with the first buffer in the ring until
 * reaching the current file again where it searched from the
 * beginning of the buffer up to its current dot.
 * Searching backwards does the reverse.
 *
 * N also differs from S in the interpretation of two arguments.
 * Using two arguments the search will be bounded between the
 * buffer with number <from>, up to the buffer with number
 * <to>.
 * When specifying buffer ranges, the entire buffers are searched
 * from beginning to end.
 * <from> may be greater than <to> in which case, searching starts
 * at the end of buffer <from> and continues backwards until the
 * beginning of buffer <to> has been reached.
 * Furthermore as with all buffer numbers, the buffer ring
 * is considered a circular structure and it is possible
 * to search over buffer ring boundaries by specifying
 * buffer numbers greater than the number of buffers in the
 * ring.
 */
TECO_DEFINE_STATE_SEARCH(teco_state_search_all,
	.initial_cb = (teco_state_initial_cb_t)teco_state_search_all_initial
);

static teco_state_t *
teco_state_search_kill_done(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	if (ctx->mode > TECO_MODE_NORMAL)
		return &teco_state_start;

	teco_qreg_t *search_reg = teco_qreg_table_find(&teco_qreg_table_globals, "_", 1);
	g_assert(search_reg != NULL);

	teco_int_t search_state;
	if (!teco_state_search_done(ctx, str, error) ||
	    !search_reg->vtable->get_integer(search_reg, &search_state, error))
		return NULL;

	if (teco_is_failure(search_state))
		return &teco_state_start;

	gint dot = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);

	teco_interface_ssm(SCI_BEGINUNDOACTION, 0, 0);
	if (teco_search_parameters.dot < dot) {
		/* kill forwards */
		gint anchor = teco_interface_ssm(SCI_GETANCHOR, 0, 0);

		if (teco_current_doc_must_undo())
			undo__teco_interface_ssm(SCI_GOTOPOS, dot, 0);
		teco_interface_ssm(SCI_GOTOPOS, anchor, 0);

		teco_interface_ssm(SCI_DELETERANGE, teco_search_parameters.dot,
		                   anchor - teco_search_parameters.dot);
	} else {
		/* kill backwards */
		teco_interface_ssm(SCI_DELETERANGE, dot, teco_search_parameters.dot - dot);
	}
	teco_interface_ssm(SCI_ENDUNDOACTION, 0, 0);
	teco_ring_dirtify();

	/* NOTE: An undo action is not always created. */
	if (teco_current_doc_must_undo() &&
	    teco_search_parameters.dot != dot)
		undo__teco_interface_ssm(SCI_UNDO, 0, 0);

	return &teco_state_start;
}

/*$ FK
 * FK[pattern]$ -- Delete up to occurrence of pattern
 * [n]FK[pattern]$
 * -FK[pattern]$
 * from,toFK[pattern]$
 * :FK[pattern]$ -> Success|Failure
 * [n]:FK[pattern]$ -> Success|Failure
 * -:FK[pattern]$ -> Success|Failure
 * from,to:FK[pattern]$ -> Success|Failure
 *
 * FK searches for <pattern> just like the regular search
 * command (S) but when found deletes all text from dot
 * up to but not including the found text instance.
 * When searching backwards the characters beginning after
 * the occurrence of <pattern> up to dot are deleted.
 *
 * In interactive mode, deletion is not performed
 * as-you-type but only on command termination.
 */
TECO_DEFINE_STATE_SEARCH(teco_state_search_kill);

static teco_state_t *
teco_state_search_delete_done(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	if (ctx->mode > TECO_MODE_NORMAL)
		return &teco_state_start;

	teco_qreg_t *search_reg = teco_qreg_table_find(&teco_qreg_table_globals, "_", 1);
	g_assert(search_reg != NULL);

	teco_int_t search_state;
	if (!teco_state_search_done(ctx, str, error) ||
	    !search_reg->vtable->get_integer(search_reg, &search_state, error))
		return NULL;

	if (teco_is_success(search_state)) {
		teco_interface_ssm(SCI_BEGINUNDOACTION, 0, 0);
		teco_interface_ssm(SCI_REPLACESEL, 0, (sptr_t)"");
		teco_interface_ssm(SCI_ENDUNDOACTION, 0, 0);
		teco_ring_dirtify();

		if (teco_current_doc_must_undo())
			undo__teco_interface_ssm(SCI_UNDO, 0, 0);
	}

	return &teco_state_start;
}

/*$ FD
 * FD[pattern]$ -- Delete occurrence of pattern
 * [n]FD[pattern]$
 * -FD[pattern]$
 * from,toFD[pattern]$
 * :FD[pattern]$ -> Success|Failure
 * [n]:FD[pattern]$ -> Success|Failure
 * -:FD[pattern]$ -> Success|Failure
 * from,to:FD[pattern]$ -> Success|Failure
 *
 * Searches for <pattern> just like the regular search command
 * (S) but when found deletes the entire occurrence.
 */
TECO_DEFINE_STATE_SEARCH(teco_state_search_delete);

static gboolean
teco_state_replace_insert_initial(teco_machine_main_t *ctx, GError **error)
{
	if (ctx->mode == TECO_MODE_NORMAL)
		teco_undo_guint(ctx->expectstring.machine.codepage) = teco_interface_get_codepage();
	return TRUE;
}

/*
 * FIXME: Could be static
 */
TECO_DEFINE_STATE_INSERT(teco_state_replace_insert,
	.initial_cb = (teco_state_initial_cb_t)teco_state_replace_insert_initial
);

static teco_state_t *
teco_state_replace_ignore_done(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	return &teco_state_start;
}

/*
 * FIXME: Could be static
 */
TECO_DEFINE_STATE_EXPECTSTRING(teco_state_replace_ignore);

static teco_state_t *
teco_state_replace_done(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	if (ctx->mode > TECO_MODE_NORMAL)
		return &teco_state_replace_ignore;

	teco_qreg_t *search_reg = teco_qreg_table_find(&teco_qreg_table_globals, "_", 1);
	g_assert(search_reg != NULL);

	teco_int_t search_state;
	if (!teco_state_search_delete_done(ctx, str, error) ||
	    !search_reg->vtable->get_integer(search_reg, &search_state, error))
		return NULL;

	return teco_is_success(search_state) ? &teco_state_replace_insert
	                                     : &teco_state_replace_ignore;
}

/*$ FS
 * FS[pattern]$[string]$ -- Search and replace
 * [n]FS[pattern]$[string]$
 * -FS[pattern]$[string]$
 * from,toFS[pattern]$[string]$
 * :FS[pattern]$[string]$ -> Success|Failure
 * [n]:FS[pattern]$[string]$ -> Success|Failure
 * -:FS[pattern]$[string]$ -> Success|Failure
 * from,to:FS[pattern]$[string]$ -> Success|Failure
 *
 * Search for <pattern> just like the regular search command
 * (S) does but replace it with <string> if found.
 * If <string> is empty, the occurrence will always be
 * deleted so \(lqFS[pattern]$$\(rq is similar to
 * \(lqFD[pattern]$\(rq.
 * The global replace register is \fBnot\fP touched
 * by the FS command.
 *
 * In interactive mode, the replacement will be performed
 * immediately and interactively.
 */
TECO_DEFINE_STATE_SEARCH(teco_state_replace,
	.expectstring.last = FALSE
);

/*
 * FIXME: TECO_DEFINE_STATE_INSERT() already defines a done_cb(),
 * so we had to name this differently.
 * Perhaps it simply shouldn't define it.
 */
static teco_state_t *
teco_state_replace_default_insert_done_overwrite(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	if (ctx->mode > TECO_MODE_NORMAL)
		return &teco_state_start;

	teco_qreg_t *replace_reg = teco_qreg_table_find(&teco_qreg_table_globals, "-", 1);
	g_assert(replace_reg != NULL);

	if (str->len > 0) {
		if (!replace_reg->vtable->undo_set_string(replace_reg, error) ||
		    !replace_reg->vtable->set_string(replace_reg, str->data, str->len,
		                                     teco_default_codepage(), error))
			return NULL;
	} else {
		g_auto(teco_string_t) replace_str = {NULL, 0};
		if (!replace_reg->vtable->get_string(replace_reg, &replace_str.data, &replace_str.len,
		                                     NULL, error) ||
		    (replace_str.len > 0 && !teco_state_insert_process(ctx, &replace_str, replace_str.len, error)))
			return NULL;
	}

	return &teco_state_start;
}

/*
 * FIXME: Could be static
 */
TECO_DEFINE_STATE_INSERT(teco_state_replace_default_insert,
	.initial_cb = NULL,
	.expectstring.done_cb = teco_state_replace_default_insert_done_overwrite
);

static teco_state_t *
teco_state_replace_default_ignore_done(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	if (ctx->mode > TECO_MODE_NORMAL ||
	    !str->len)
		return &teco_state_start;

	teco_qreg_t *replace_reg = teco_qreg_table_find(&teco_qreg_table_globals, "-", 1);
	g_assert(replace_reg != NULL);

	if (!replace_reg->vtable->undo_set_string(replace_reg, error) ||
	    !replace_reg->vtable->set_string(replace_reg, str->data, str->len,
	                                     teco_default_codepage(), error))
		return NULL;

	return &teco_state_start;
}

/*
 * FIXME: Could be static
 */
TECO_DEFINE_STATE_EXPECTSTRING(teco_state_replace_default_ignore);

static teco_state_t *
teco_state_replace_default_done(teco_machine_main_t *ctx, const teco_string_t *str, GError **error)
{
	if (ctx->mode > TECO_MODE_NORMAL)
		return &teco_state_replace_default_ignore;

	teco_qreg_t *search_reg = teco_qreg_table_find(&teco_qreg_table_globals, "_", 1);
	g_assert(search_reg != NULL);

	teco_int_t search_state;
	if (!teco_state_search_delete_done(ctx, str, error) ||
	    !search_reg->vtable->get_integer(search_reg, &search_state, error))
		return NULL;

	return teco_is_success(search_state) ? &teco_state_replace_default_insert
	                                     : &teco_state_replace_default_ignore;
}

/*$ FR
 * FR[pattern]$[string]$ -- Search and replace with default
 * [n]FR[pattern]$[string]$
 * -FR[pattern]$[string]$
 * from,toFR[pattern]$[string]$
 * :FR[pattern]$[string]$ -> Success|Failure
 * [n]:FR[pattern]$[string]$ -> Success|Failure
 * -:FR[pattern]$[string]$ -> Success|Failure
 * from,to:FR[pattern]$[string]$ -> Success|Failure
 *
 * The FR command is similar to the FS command.
 * It searches for <pattern> just like the regular search
 * command (S) and replaces the occurrence with <string>
 * similar to what FS does.
 * It differs from FS in the fact that the replacement
 * string is saved in the global replacement register
 * \(lq-\(rq.
 * If <string> is empty the string in the global replacement
 * register is implied instead.
 */
TECO_DEFINE_STATE_SEARCH(teco_state_replace_default,
	.expectstring.last = FALSE
);
