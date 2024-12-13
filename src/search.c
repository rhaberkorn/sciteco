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

/*$ ^X search-mode
 * mode^X -- Set or get search mode flag
 * -^X
 * ^X -> mode
 *
 * The search mode is interpreted as a TECO boolean.
 * A true value (smaller than zero) configures case-sensitive searches,
 * while a false value (larger than or equal to zero) configures case-insensitive
 * searches.
 * \(lq-^X\(rq is equivalent to \(lq-1^X\(rq and also enables case-sensitive searches.
 * Searches are case-insensitive by default.
 *
 * An alternative way to access the search mode is via the \(lq^X\(rq local Q-Register.
 * Consequently, the search mode is local to the current macro invocation frame,
 * unless the macro call was colon-modified.
 */
void
teco_state_control_search_mode(teco_machine_main_t *ctx, GError **error)
{
	if (!teco_expressions_eval(FALSE, error))
		return;

	teco_qreg_t *reg = teco_qreg_table_find(ctx->qreg_table_locals, "\x18", 1); /* ^X */
	g_assert(reg != NULL);
	teco_bool_t search_mode;

	if (!teco_expressions_args() && teco_num_sign > 0) {
		if (!reg->vtable->get_integer(reg, &search_mode, error))
			return;
		teco_expressions_push(search_mode);
	} else {
		if (!teco_expressions_pop_num_calc(&search_mode, teco_num_sign, error) ||
		    !reg->vtable->undo_set_integer(reg, error) ||
		    !reg->vtable->set_integer(reg, search_mode, error))
			return;
	}
}

static gboolean
teco_state_search_initial(teco_machine_main_t *ctx, GError **error)
{
	if (ctx->mode > TECO_MODE_NORMAL)
		return TRUE;

	teco_machine_stringbuilding_set_codepage(&ctx->expectstring.machine,
	                                         teco_interface_get_codepage());

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
	TECO_SEARCH_STATE_CTL,
	TECO_SEARCH_STATE_ESCAPE,
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
 * @param qreg_machine State machine for parsing Q-Regs (^EGq).
 * @param codepage The codepage of pattern.
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
                  teco_machine_qregspec_t *qreg_machine,
                  guint codepage, gboolean escape_default, GError **error)
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
				gsize len = codepage == SC_CP_UTF8
						? g_utf8_next_char(pattern->data) - pattern->data : 1;
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
			gsize len;
			gunichar chr;

			if (codepage == SC_CP_UTF8) {
				len = g_utf8_next_char(pattern->data) - pattern->data;
				chr = g_utf8_get_char(pattern->data);
			} else {
				len = 1;
				chr = *pattern->data;
			}
			switch (teco_machine_qregspec_input(qreg_machine,
			                                    chr, &reg, NULL, error)) {
			case TECO_MACHINE_QREGSPEC_ERROR:
				return NULL;

			case TECO_MACHINE_QREGSPEC_MORE:
				/* incomplete, but consume byte */
				pattern->data += len;
				pattern->len -= len;
				continue;

			case TECO_MACHINE_QREGSPEC_DONE:
				teco_machine_qregspec_reset(qreg_machine);

				g_auto(teco_string_t) str = {NULL, 0};
				if (!reg->vtable->get_string(reg, &str.data, &str.len, NULL, error))
					return NULL;

				pattern->data += len;
				pattern->len -= len;
				*state = TECO_SEARCH_STATE_START;
				return g_regex_escape_string(str.data ? : "", str.len);
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
 *                Modifies the pointer to point after the last
 *                successfully scanned character, so it can be
 *                called recursively. It may also point to the
 *                terminating null byte after the call.
 * @param qreg_machine State machine for parsing Q-Regs (^EGq).
 * @param codepage The codepage of pattern.
 * @param single_expr Whether to scan a single pattern
 *                    expression or an arbitrary sequence.
 * @param error A GError.
 * @return The regular expression string or NULL in case of GError.
 *         Must be freed with g_free().
 */
static gchar *
teco_pattern2regexp(teco_string_t *pattern, teco_machine_qregspec_t *qreg_machine,
                    guint codepage, gboolean single_expr, GError **error)
{
	teco_search_state_t state = TECO_SEARCH_STATE_START;
	g_auto(teco_string_t) re = {NULL, 0};

	do {
		/*
		 * Previous character was caret.
		 * Make sure it is handled like a control character.
		 * This is necessary even though we have string building activated,
		 * to support constructs like ^Q^Q (typed with carets) in order to
		 * quote pattern matching characters.
		 */
		if (state == TECO_SEARCH_STATE_CTL) {
			*pattern->data = TECO_CTL_KEY(g_ascii_toupper(*pattern->data));
			state = TECO_SEARCH_STATE_START;
		}

		/*
		 * First check whether it is a class.
		 * This will not treat individual characters
		 * as classes, so we do not convert them to regexp
		 * classes unnecessarily.
		 */
		g_autofree gchar *temp;
		temp = teco_class2regexp(&state, pattern, qreg_machine,
		                         codepage, FALSE, error);
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
			case '^':
				state = TECO_SEARCH_STATE_CTL;
				break;
			case TECO_CTL_KEY('Q'):
			case TECO_CTL_KEY('R'):
				state = TECO_SEARCH_STATE_ESCAPE;
				break;
			case TECO_CTL_KEY('X'):
				teco_string_append_c(&re, '.');
				break;
			case TECO_CTL_KEY('N'):
				state = TECO_SEARCH_STATE_NOT;
				break;
			default:
				state = TECO_SEARCH_STATE_ESCAPE;
				continue;
			}
			break;

		case TECO_SEARCH_STATE_ESCAPE: {
			state = TECO_SEARCH_STATE_START;
			gsize len = codepage == SC_CP_UTF8
					? g_utf8_next_char(pattern->data) - pattern->data : 1;
			/* the allocation could theoretically be avoided by escaping char-wise */
			g_autofree gchar *escaped = g_regex_escape_string(pattern->data, len);
			teco_string_append(&re, escaped, strlen(escaped));
			pattern->data += len;
			pattern->len -= len;
			continue;
		}

		case TECO_SEARCH_STATE_NOT: {
			state = TECO_SEARCH_STATE_START;
			g_autofree gchar *temp;
			temp = teco_class2regexp(&state, pattern, qreg_machine,
			                         codepage, TRUE, error);
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
			g_autofree gchar *temp;
			temp = teco_pattern2regexp(pattern, qreg_machine,
			                           codepage, TRUE, error);
			if (!temp)
				return NULL;
			if (!*temp)
				/* a complete expression is strictly required */
				return g_strdup("");

			/* don't capture this group - it's not included in ^Y */
			teco_string_append(&re, "(?:", 3);
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
				g_autofree gchar *temp;
				temp = teco_pattern2regexp(pattern, qreg_machine,
				                           codepage, TRUE, error);
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

TECO_DEFINE_UNDO_OBJECT_OWN(ranges, teco_range_t *, g_free);

#define teco_undo_ranges_own(VAR) \
	(*teco_undo_object_ranges_push(&(VAR)))

static teco_range_t *
teco_get_ranges(const GMatchInfo *match_info, gsize offset, guint *count)
{
	*count = g_match_info_get_match_count(match_info);
	teco_range_t *ranges = g_new(teco_range_t, *count);

	for (gint i = 0; i < *count; i++) {
		gint from, to;
		g_match_info_fetch_pos(match_info, i, &from, &to);
		ranges[i].from = offset+MAX(from, 0);
		ranges[i].to = offset+MAX(to, 0);
	}

	return ranges;
}

static gboolean
teco_do_search(GRegex *re, gsize from, gsize to, gint *count, GError **error)
{
	g_autoptr(GMatchInfo) info = NULL;
	/* NOTE: can return NULL pointer for completely new and empty documents */
	const gchar *buffer = (const gchar *)teco_interface_ssm(SCI_GETRANGEPOINTER, from, to-from) ? : "";
	GError *tmp_error = NULL;

	/*
	 * NOTE: The return boolean does NOT signal whether an error was generated.
	 */
	g_regex_match_full(re, buffer, to-from, 0, 0, &info, &tmp_error);
	if (tmp_error) {
		g_propagate_error(error, tmp_error);
		return FALSE;
	}

	guint num_ranges = 0;
	teco_range_t *matched_ranges = NULL;

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
			matched_ranges = teco_get_ranges(info, from, &num_ranges);
	} else {
		/* only keep the last `count' matches, in a circular stack */
		typedef struct {
			guint num_ranges;
			teco_range_t *ranges;
		} teco_match_t;

		guint matched_num = -*count;
		gsize matched_size = sizeof(teco_match_t[matched_num]);

		/*
		 * matched_size could overflow.
		 * NOTE: Glib 2.48 has g_size_checked_mul() which uses
		 * compiler intrinsics.
		 */
		if (matched_size / sizeof(teco_match_t) != matched_num)
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

		/*
		 * NOTE: This needs to be deep-freed, which does not currently
		 * happen automatically.
		 */
		g_autofree teco_match_t *matched = g_malloc0(matched_size);

		gint matched_total = 0, i = 0;

		while (g_match_info_matches(info)) {
			g_free(matched[i].ranges);
			matched[i].ranges = teco_get_ranges(info, from, &matched[i].num_ranges);

			/*
			 * NOTE: The return boolean does NOT signal whether an error was generated.
			 */
			g_match_info_next(info, &tmp_error);
			if (tmp_error) {
				g_propagate_error(error, tmp_error);
				for (int i = 0; i < matched_num; i++)
					g_free(matched[i].ranges);
				return FALSE;
			}

			i = ++matched_total % -(*count);
		}

		*count = MIN(*count + matched_total, 0);
		if (!*count) {
			/* successful -> i points to stack bottom */
			num_ranges = matched[i].num_ranges;
			matched_ranges = matched[i].ranges;
			matched[i].ranges = NULL;
		}

		for (int i = 0; i < matched_num; i++)
			g_free(matched[i].ranges);
	}

	if (matched_ranges) {
		/* match success */
		teco_undo_ranges_own(teco_ranges) = matched_ranges;
		teco_undo_guint(teco_ranges_count) = num_ranges;
		g_assert(teco_ranges_count > 0);

		teco_interface_ssm(SCI_SETSEL, matched_ranges[0].from, matched_ranges[0].to);
	}

	return TRUE;
}

static gboolean
teco_state_search_process(teco_machine_main_t *ctx, const teco_string_t *str, gsize new_chars, GError **error)
{
	/* FIXME: Should G_REGEX_OPTIMIZE be added under certain circumstances? */
	GRegexCompileFlags flags = G_REGEX_MULTILINE | G_REGEX_DOTALL;

	teco_qreg_t *reg = teco_qreg_table_find(ctx->qreg_table_locals, "\x18", 1); /* ^X */
	g_assert(reg != NULL);
	teco_bool_t search_mode;
	if (!reg->vtable->get_integer(reg, &search_mode, error))
		return FALSE;
	if (teco_is_failure(search_mode))
		flags |= G_REGEX_CASELESS;

	if (ctx->modifier_colon == 2)
		flags |= G_REGEX_ANCHORED;

	/* this is set in teco_state_search_initial() */
	if (ctx->expectstring.machine.codepage != SC_CP_UTF8) {
		/* single byte encoding */
		flags |= G_REGEX_RAW;
	} else if (!teco_string_validate_utf8(str)) {
		/*
		 * While SciTECO code is always guaranteed to be in valid UTF-8,
		 * the result of string building may not (eg. if ^EQq inserts garbage).
		 */
		g_set_error_literal(error, TECO_ERROR, TECO_ERROR_CODEPOINT,
		                    "Invalid UTF-8 byte sequence in search pattern");
		return FALSE;
	}

	if (teco_current_doc_must_undo())
		undo__teco_interface_ssm(SCI_SETSEL,
		                         teco_interface_ssm(SCI_GETANCHOR, 0, 0),
		                         teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0));

	teco_qreg_t *search_reg = teco_qreg_table_find(&teco_qreg_table_globals, "_", 1);
	g_assert(search_reg != NULL);
	if (!search_reg->vtable->undo_set_integer(search_reg, error) ||
	    !search_reg->vtable->set_integer(search_reg, TECO_FAILURE, error))
		return FALSE;

	g_autoptr(teco_machine_qregspec_t) qreg_machine;
	qreg_machine = teco_machine_qregspec_new(TECO_QREG_REQUIRED, ctx->qreg_table_locals, FALSE);

	g_autoptr(GRegex) re = NULL;
	teco_string_t pattern = *str;
	g_autofree gchar *re_pattern;
	/* NOTE: teco_pattern2regexp() modifies str pointer */
	re_pattern = teco_pattern2regexp(&pattern, qreg_machine,
	                                 ctx->expectstring.machine.codepage, FALSE, error);
	if (!re_pattern)
		return FALSE;
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

	if (teco_machine_main_eval_colon(ctx) > 0)
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

/*$ S search pattern compare
 * [n]S[pattern]$ -- Search for pattern
 * -S[pattern]$
 * from,toS[pattern]$
 * [n]:S[pattern]$ -> Success|Failure
 * -:S[pattern]$ -> Success|Failure
 * from,to:S[pattern]$ -> Success|Failure
 * [n]::S[pattern]$ -> Success|Failure
 * -::S[pattern]$ -> Success|Failure
 * from,to::S[pattern]$ -> Success|Failure
 *
 * Search for <pattern> in the current buffer/Q-Register.
 * Search order and range depends on the arguments given.
 * By default without any arguments, \fBS\fP will search forward
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
 * Alternatively, \fBS\fP may be colon-modified in which case it returns
 * a condition boolean that may be directly evaluated by a
 * conditional or break-command.
 *
 * When modified with two colons, the search will be anchored in addition
 * to returning a condition boolean, i.e. it can be used to perform a string
 * comparison.
 * \(lq::S\(rq without arguments compares the pattern against the current
 * buffer position.
 * With a single positive integer <n>, \(lq::S\(rq matches <n> repititions
 * of the given pattern.
 * With a negative integer <n>, \(lq::S\(rq will match the \fIn\fP-th last
 * occurrence of the pattern from the beginning of the buffer
 * (which is not really useful).
 * With two integer arguments, \(lq::S\(rq compares the pattern against
 * the corresponding part of the buffer.
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

	teco_machine_stringbuilding_set_codepage(&ctx->expectstring.machine,
	                                         teco_interface_get_codepage());

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
 * (\fBS\fP) but will continue to search for occurrences of
 * pattern when the end or beginning of the current buffer
 * is reached.
 * Occurrences of <pattern> spanning over buffer boundaries
 * will not be found.
 * When searching forward \fBN\fP will start in the current buffer
 * at dot, continue with the next buffer in the ring searching
 * the entire buffer until it reaches the end of the buffer
 * ring, continue with the first buffer in the ring until
 * reaching the current file again where it searched from the
 * beginning of the buffer up to its current dot.
 * Searching backwards does the reverse.
 *
 * \fBN\fP also differs from \fBS\fP in the interpretation of two arguments.
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
/*
 * Since N inherits the double-colon semantics from S,
 * f,t::N will match at the beginning of the given buffers.
 * [n]::N will behave similar to [n]::S, but can search across
 * buffer boundaries.
 * This is probably not very useful in practice, so it's not documented.
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

	sptr_t dot = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);

	teco_interface_ssm(SCI_BEGINUNDOACTION, 0, 0);
	if (teco_search_parameters.dot < dot) {
		/* kill forwards */
		sptr_t anchor = teco_interface_ssm(SCI_GETANCHOR, 0, 0);
		gsize len = anchor - teco_search_parameters.dot;

		if (teco_current_doc_must_undo())
			undo__teco_interface_ssm(SCI_GOTOPOS, dot, 0);
		teco_interface_ssm(SCI_GOTOPOS, anchor, 0);

		teco_interface_ssm(SCI_DELETERANGE, teco_search_parameters.dot, len);

		/* NOTE: An undo action is not always created. */
		if (teco_current_doc_must_undo() &&
		    teco_search_parameters.dot != anchor)
			undo__teco_interface_ssm(SCI_UNDO, 0, 0);

		/* fix up ranges (^Y) */
		for (guint i = 0; i < teco_ranges_count; i++) {
			teco_ranges[i].from -= len;
			teco_ranges[i].to -= len;
		}
	} else {
		/* kill backwards */
		teco_interface_ssm(SCI_DELETERANGE, dot, teco_search_parameters.dot - dot);

		/* NOTE: An undo action is not always created. */
		if (teco_current_doc_must_undo() &&
		    teco_search_parameters.dot != dot)
			undo__teco_interface_ssm(SCI_UNDO, 0, 0);
	}
	teco_interface_ssm(SCI_ENDUNDOACTION, 0, 0);
	teco_ring_dirtify();

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
 * \fBFK\fP searches for <pattern> just like the regular search
 * command (\fBS\fP) but when found, deletes all text from dot
 * up to but not including the found text instance.
 * When searching backwards the characters beginning after
 * the occurrence of <pattern> up to dot are deleted.
 *
 * In interactive mode, deletion is not performed
 * as-you-type but only on command termination.
 */
/*
 * ::FK is possible but doesn't make much sense, so it's undocumented.
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
 * [n]FD[pattern]$ -- Delete occurrence of pattern
 * -FD[pattern]$
 * from,toFD[pattern]$
 * [n]:FD[pattern]$ -> Success|Failure
 * -:FD[pattern]$ -> Success|Failure
 * from,to:FD[pattern]$ -> Success|Failure
 * [n]::FD[pattern]$ -> Success|Failure
 * -::FD[pattern]$ -> Success|Failure
 * from,to::FD[pattern]$ -> Success|Failure
 *
 * Searches for <pattern> just like the regular search command
 * (\fBS\fP) but when found deletes the entire occurrence.
 */
TECO_DEFINE_STATE_SEARCH(teco_state_search_delete);

static gboolean
teco_state_replace_insert_initial(teco_machine_main_t *ctx, GError **error)
{
	if (ctx->mode == TECO_MODE_NORMAL)
		teco_machine_stringbuilding_set_codepage(&ctx->expectstring.machine,
		                                         teco_interface_get_codepage());
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
 * [n]FS[pattern]$[string]$ -- Search and replace
 * -FS[pattern]$[string]$
 * from,toFS[pattern]$[string]$
 * [n]:FS[pattern]$[string]$ -> Success|Failure
 * -:FS[pattern]$[string]$ -> Success|Failure
 * from,to:FS[pattern]$[string]$ -> Success|Failure
 * [n]::FS[pattern]$[string]$ -> Success|Failure
 * -::FS[pattern]$[string]$ -> Success|Failure
 * from,to::FS[pattern]$[string]$ -> Success|Failure
 *
 * Search for <pattern> just like the regular search command
 * (\fBS\fP) does but replace it with <string> if found.
 * If <string> is empty, the occurrence will always be
 * deleted so \(lqFS[pattern]\fB$$\fP\(rq is similar to
 * \(lqFD[pattern]$\(rq.
 * The global replace register is \fBnot\fP touched
 * by the \fBFS\fP command.
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
 * [n]FR[pattern]$[string]$ -- Search and replace with default
 * -FR[pattern]$[string]$
 * from,toFR[pattern]$[string]$
 * [n]:FR[pattern]$[string]$ -> Success|Failure
 * -:FR[pattern]$[string]$ -> Success|Failure
 * from,to:FR[pattern]$[string]$ -> Success|Failure
 * [n]::FR[pattern]$[string]$ -> Success|Failure
 * -::FR[pattern]$[string]$ -> Success|Failure
 * from,to::FR[pattern]$[string]$ -> Success|Failure
 *
 * The \fBFR\fP command is similar to the \fBFS\fP command.
 * It searches for <pattern> just like the regular search
 * command (\fBS\fP) and replaces the occurrence with <string>
 * similar to what \fBFS\fP does.
 * It differs from \fBFS\fP in the fact that the replacement
 * string is saved in the global replacement register
 * \(lq-\(rq.
 * If <string> is empty the string in the global replacement
 * register is implied instead.
 */
TECO_DEFINE_STATE_SEARCH(teco_state_replace_default,
	.expectstring.last = FALSE
);
