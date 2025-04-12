/*
 * Copyright (C) 2012-2025 Robin Haberkorn
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
/**
 * @file Movement \b and corresponding deletion commands.
 * This also includes the lines to glyphs conversion command.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib.h>
#include <glib/gstdio.h>

#include "sciteco.h"
#include "string-utils.h"
#include "expressions.h"
#include "interface.h"
#include "parser.h"
#include "ring.h"
#include "undo.h"
#include "error.h"
#include "core-commands.h"
#include "move-commands.h"

/*$ J jump
 * [position]J -- Go to position in buffer
 * [position]:J -> Success|Failure
 *
 * Sets dot to <position>.
 * If <position> is omitted, 0 is implied and \(lqJ\(rq will
 * go to the beginning of the buffer.
 *
 * If <position> is outside the range of the buffer, the
 * command yields an error.
 * If colon-modified, the command will instead return a
 * condition boolean signalling whether the position could
 * be changed or not.
 */
void
teco_state_start_jump(teco_machine_main_t *ctx, GError **error)
{
	teco_int_t v;

	if (!teco_expressions_pop_num_calc(&v, 0, error))
		return;

	gssize pos = teco_interface_glyphs2bytes(v);
	if (pos >= 0) {
		if (teco_current_doc_must_undo())
			undo__teco_interface_ssm(SCI_GOTOPOS,
			                         teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0), 0);
		teco_interface_ssm(SCI_GOTOPOS, pos, 0);

		if (teco_machine_main_eval_colon(ctx) > 0)
			teco_expressions_push(TECO_SUCCESS);
	} else if (teco_machine_main_eval_colon(ctx) > 0) {
		teco_expressions_push(TECO_FAILURE);
	} else {
		teco_error_move_set(error, "J");
		return;
	}
}

static teco_bool_t
teco_move_chars(teco_int_t n)
{
	sptr_t pos = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);
	gssize next_pos = teco_interface_glyphs2bytes_relative(pos, n);
	if (next_pos < 0)
		return TECO_FAILURE;

	teco_interface_ssm(SCI_GOTOPOS, next_pos, 0);
	if (teco_current_doc_must_undo())
		undo__teco_interface_ssm(SCI_GOTOPOS, pos, 0);

	return TECO_SUCCESS;
}

/*$ C move
 * [n]C -- Move dot <n> characters
 * -C
 * [n]:C -> Success|Failure
 *
 * Adds <n> to dot. 1 or -1 is implied if <n> is omitted.
 * Fails if <n> would move dot off-page.
 * The colon modifier results in a success-boolean being
 * returned instead.
 */
void
teco_state_start_move(teco_machine_main_t *ctx, GError **error)
{
	teco_int_t v;

	if (!teco_expressions_pop_num_calc(&v, teco_num_sign, error))
		return;

	teco_bool_t rc = teco_move_chars(v);
	if (teco_machine_main_eval_colon(ctx) > 0) {
		teco_expressions_push(rc);
	} else if (teco_is_failure(rc)) {
		teco_error_move_set(error, "C");
		return;
	}
}

/*$ R reverse
 * [n]R -- Move dot <n> characters backwards
 * -R
 * [n]:R -> Success|Failure
 *
 * Subtracts <n> from dot.
 * It is equivalent to \(lq-nC\(rq.
 */
void
teco_state_start_reverse(teco_machine_main_t *ctx, GError **error)
{
	teco_int_t v;

	if (!teco_expressions_pop_num_calc(&v, teco_num_sign, error))
		return;

	teco_bool_t rc = teco_move_chars(-v);
	if (teco_machine_main_eval_colon(ctx) > 0) {
		teco_expressions_push(rc);
	} else if (teco_is_failure(rc)) {
		teco_error_move_set(error, "R");
		return;
	}
}

static teco_bool_t
teco_move_lines(teco_int_t n)
{
	sptr_t pos = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);
	sptr_t line = teco_interface_ssm(SCI_LINEFROMPOSITION, pos, 0) + n;

	if (!teco_validate_line(line))
		return TECO_FAILURE;

	teco_interface_ssm(SCI_GOTOLINE, line, 0);
	if (teco_current_doc_must_undo())
		undo__teco_interface_ssm(SCI_GOTOPOS, pos, 0);

	return TECO_SUCCESS;
}

/*$ L line
 * [n]L -- Move dot <n> lines forwards
 * -L
 * [n]:L -> Success|Failure
 *
 * Move dot to the beginning of the line specified
 * relatively to the current line.
 * Therefore a value of 0 for <n> goes to the
 * beginning of the current line, 1 will go to the
 * next line, -1 to the previous line etc.
 * If <n> is omitted, 1 or -1 is implied depending on
 * the sign prefix.
 *
 * If <n> would move dot off-page, the command yields
 * an error.
 * The colon-modifer results in a condition boolean
 * being returned instead.
 */
void
teco_state_start_line(teco_machine_main_t *ctx, GError **error)
{
	teco_int_t v;

	if (!teco_expressions_pop_num_calc(&v, teco_num_sign, error))
		return;

	teco_bool_t rc = teco_move_lines(v);
	if (teco_machine_main_eval_colon(ctx) > 0) {
		teco_expressions_push(rc);
	} else if (teco_is_failure(rc)) {
		teco_error_move_set(error, "L");
		return;
	}
}

/*$ B backwards
 * [n]B -- Move dot <n> lines backwards
 * -B
 * [n]:B -> Success|Failure
 *
 * Move dot to the beginning of the line <n>
 * lines before the current one.
 * It is equivalent to \(lq-nL\(rq.
 */
void
teco_state_start_back(teco_machine_main_t *ctx, GError **error)
{
	teco_int_t v;

	if (!teco_expressions_pop_num_calc(&v, teco_num_sign, error))
		return;

	teco_bool_t rc = teco_move_lines(-v);
	if (teco_machine_main_eval_colon(ctx) > 0) {
		teco_expressions_push(rc);
	} else if (teco_is_failure(rc)) {
		teco_error_move_set(error, "B");
		return;
	}
}

/**
 * Find the beginning or end of a word.
 *
 * This first skips word-characters, followed by non-word characters
 * as configured by SCI_SETWORDCHARS.
 * If end_of_word is TRUE, the order is swapped.
 *
 * @note This implementation has a constant/maximum number of Scintilla
 * messages, compared to using SCI_WORDENDPOSITION.
 * This pays out only beginning at n > 3, though.
 * But most importantly SCI_WORDENDPOSITION(p, FALSE) does not actually skip
 * over all non-word characters.
 *
 * @param pos Start position for search.
 *  The result is also stored into this variable.
 * @param n How many words to skip forwards or backwards.
 * @param end_of_word Whether to search for the end or beginning of words.
 * @return FALSE if there aren't enough words in the buffer.
 */
static gboolean
teco_find_words(gsize *pos, teco_int_t n, gboolean end_of_word)
{
	if (!n)
		return TRUE;

	g_auto(teco_string_t) wchars;
	wchars.len = teco_interface_ssm(SCI_GETWORDCHARS, 0, 0);
	wchars.data = g_malloc(wchars.len + 1);
	teco_interface_ssm(SCI_GETWORDCHARS, 0, (sptr_t)wchars.data);
	wchars.data[wchars.len] = '\0';

	sptr_t gap = teco_interface_ssm(SCI_GETGAPPOSITION, 0, 0);

	if (n > 0) {
		/* scan forward */
		gsize len = teco_interface_ssm(SCI_GETLENGTH, 0, 0);
		gsize range_len = gap > *pos ? gap - *pos : len - *pos;
		if (!range_len)
			return FALSE;
		const gchar *buffer, *p;
		p = buffer = (const gchar *)teco_interface_ssm(SCI_GETRANGEPOINTER, *pos, range_len);

		while (n--) {
			gboolean skip_word = !end_of_word;

			for (;;) {
				if (*pos == len)
					/* end of document */
					return n == 0;
				if (p-buffer >= range_len) {
					g_assert(*pos == gap);
					range_len = len - gap;
					p = buffer = (const gchar *)teco_interface_ssm(SCI_GETRANGEPOINTER, gap, range_len);
				}
				/*
				 * FIXME: Is this safe or do we have to look up Unicode code points?
				 */
				if ((!teco_string_contains(&wchars, *p)) == skip_word) {
					if (skip_word == end_of_word)
						break;
					skip_word = !skip_word;
					continue;
				}
				(*pos)++;
				p++;
			}
		}

		return TRUE;
	}

	/* scan backwards */
	gsize range_len = gap < *pos ? *pos - gap : *pos;
	if (!range_len)
		return FALSE;
	const gchar *buffer, *p;
	buffer = (const gchar *)teco_interface_ssm(SCI_GETRANGEPOINTER, *pos - range_len, range_len);
	p = buffer+range_len;

	while (n++) {
		gboolean skip_word = end_of_word;

		for (;;) {
			if (*pos == 0)
				/* beginning of document */
				return n == 0;
			if (p == buffer) {
				g_assert(*pos == gap);
				range_len = *pos;
				buffer = (const gchar *)teco_interface_ssm(SCI_GETRANGEPOINTER, 0, range_len);
				p = buffer+range_len;
			}
			/*
			 * FIXME: Is this safe or do we have to look up Unicode code points?
			 */
			if ((!teco_string_contains(&wchars, p[-1])) == skip_word) {
				if (skip_word != end_of_word)
					break;
				skip_word = !skip_word;
				continue;
			}
			(*pos)--;
			p--;
		}
	}

	return TRUE;
}

/*$ W word
 * [n]W -- Move dot <n> words forwards
 * -W
 * [n]:W -> Success|Failure
 * [n]@W
 * [n]:@W -> Success|Failure
 *
 * If <n> is positive, move dot <n> words forwards by first skipping
 * word characters, followed by non-word characters.
 * If <n> is negative, move dot <-n> words backwards by first skipping
 * non-word characters, followed by word characters.
 * This leaves dot at the beginning of words as defined by the Scintilla
 * message
 * .BR SCI_SETWORDCHARS .
 * If <n> is zero, dot is not moved.
 * If <n> is omitted, 1 or -1 is implied depending on the sign prefix.
 *
 * The command is at-modified (\fB@\fP), the order of word vs. non-word
 * character skipping is swapped, which leaves dot at the end of words.
 * It is especially useful for jumping to the end of the current word.
 *
 * If the requested word would lie beyond the range of the
 * buffer, the command yields an error.
 * If colon-modified it instead returns a condition code.
 */
/*$ P
 * [n]P -- Move dot <n> words backwards
 * -P
 * [n]:P -> Success|Failure
 * [n]@P
 * [n]:@P -> Success|Failure
 *
 * Move dot to the beginning of preceding words if <n> is positive.
 * It is completely equivalent to \(lq-\fIn\fPW\(rq.
 */
teco_state_t *
teco_state_start_words(teco_machine_main_t *ctx, const gchar *cmd, gint factor, GError **error)
{
	/*
	 * NOTE: "@" has syntactic significance in most contexts, so it's set
	 * in parse-only mode.
	 * Therefore, it must also be evaluated in parse-only mode, even though
	 * it has no syntactic significance for W.
	 */
	gboolean modifier_at = teco_machine_main_eval_at(ctx);

	if (ctx->flags.mode > TECO_MODE_NORMAL)
		return &teco_state_start;

	teco_int_t v;
	if (!teco_expressions_pop_num_calc(&v, teco_num_sign, error))
		return NULL;

	sptr_t pos = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);

	gsize word_pos = pos;
	gboolean rc = teco_find_words(&word_pos, factor*v, modifier_at);
	if (rc) {
		if (teco_current_doc_must_undo())
			undo__teco_interface_ssm(SCI_GOTOPOS, pos, 0);
		teco_interface_ssm(SCI_GOTOPOS, word_pos, 0);
	}

	if (teco_machine_main_eval_colon(ctx) > 0) {
		teco_expressions_push(teco_bool(rc));
	} else if (!rc) {
		teco_error_words_set(error, cmd);
		return NULL;
	}

	return &teco_state_start;
}

/*$ V
 * [n]V -- Delete words forwards
 * -V
 * [n]:V -> Success|Failure
 * [n]@V
 * [n]:@V -> Success|Failure
 *
 * If <n> is positive, deletes the next <n> words until the
 * beginning of the \fIn\fP'th word after the current one.
 * It is deleting exactly until the position that the equivalent
 * .B W
 * command would move to.
 *
 * \(lq@V\(rq is especially useful to remove the remainder of the
 * current word.
 */
/*$ Y
 * [n]Y -- Delete word backwards
 * -Y
 * [n]:Y -> Success|Failure
 * [n]@Y
 * [n]:@Y -> Success|Failure
 *
 * If <n> is positive, deletes the preceding <n> words until the
 * beginning of the \fIn\fP'th word before the current one.
 * It is deleting exactly until the position that the equivalent
 * .B P
 * command would move to.
 * Y is completely equivalent to \(lq-\fIn\fPV\(rq.
 */
teco_state_t *
teco_state_start_delete_words(teco_machine_main_t *ctx, const gchar *cmd, gint factor, GError **error)
{
	/*
	 * NOTE: "@" has syntactic significance in most contexts, so it's set
	 * in parse-only mode.
	 * Therefore, it must also be evaluated in parse-only mode, even though
	 * it has no syntactic significance for W.
	 */
	gboolean modifier_at = teco_machine_main_eval_at(ctx);

	if (ctx->flags.mode > TECO_MODE_NORMAL)
		return &teco_state_start;

	teco_int_t v;
	if (!teco_expressions_pop_num_calc(&v, teco_num_sign, error))
		return NULL;
	v *= factor;

	sptr_t pos = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);

	gsize start_pos = pos, end_pos = pos;
	gboolean rc = teco_find_words(v > 0 ? &end_pos : &start_pos, v, modifier_at);
	if (rc && start_pos != end_pos) {
		g_assert(start_pos < end_pos);

		teco_interface_ssm(SCI_BEGINUNDOACTION, 0, 0);
		teco_interface_ssm(SCI_DELETERANGE, start_pos, end_pos-start_pos);
		teco_interface_ssm(SCI_ENDUNDOACTION, 0, 0);

		if (teco_current_doc_must_undo()) {
			undo__teco_interface_ssm(SCI_GOTOPOS, pos, 0);
			undo__teco_interface_ssm(SCI_UNDO, 0, 0);
		}
		teco_ring_dirtify();
	}

	if (teco_machine_main_eval_colon(ctx) > 0) {
		teco_expressions_push(teco_bool(rc));
	} else if (!rc) {
		teco_error_words_set(error, cmd);
		return NULL;
	}

	return &teco_state_start;
}

static gboolean
teco_state_start_kill(teco_machine_main_t *ctx, const gchar *cmd, gboolean by_lines, GError **error)
{
	teco_bool_t rc;
	gssize from, len; /* in bytes */

	if (!teco_expressions_eval(FALSE, error))
		return FALSE;

	if (teco_expressions_args() <= 1) {
		from = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);
		if (by_lines) {
			teco_int_t line;
			if (!teco_expressions_pop_num_calc(&line, teco_num_sign, error))
				return FALSE;
			line += teco_interface_ssm(SCI_LINEFROMPOSITION, from, 0);
			len = teco_interface_ssm(SCI_POSITIONFROMLINE, line, 0) - from;
			rc = teco_bool(teco_validate_line(line));
		} else {
			teco_int_t len_glyphs;
			if (!teco_expressions_pop_num_calc(&len_glyphs, teco_num_sign, error))
				return FALSE;
			gssize to = teco_interface_glyphs2bytes_relative(from, len_glyphs);
			rc = teco_bool(to >= 0);
			len = to-from;
		}
		if (len < 0) {
			len *= -1;
			from -= len;
		}
	} else {
		teco_int_t to_glyphs = teco_expressions_pop_num(0);
		gssize to = teco_interface_glyphs2bytes(to_glyphs);
		teco_int_t from_glyphs = teco_expressions_pop_num(0);
		from = teco_interface_glyphs2bytes(from_glyphs);
		len = to - from;
		rc = teco_bool(len >= 0 && from >= 0 && to >= 0);
	}

	if (teco_machine_main_eval_colon(ctx) > 0) {
		teco_expressions_push(rc);
	} else if (teco_is_failure(rc)) {
		teco_error_range_set(error, cmd);
		return FALSE;
	}

	if (len == 0 || teco_is_failure(rc))
		return TRUE;

	if (teco_current_doc_must_undo()) {
		sptr_t pos = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);
		undo__teco_interface_ssm(SCI_GOTOPOS, pos, 0);
		undo__teco_interface_ssm(SCI_UNDO, 0, 0);
	}

	/*
	 * Should always generate an undo action.
	 */
	teco_interface_ssm(SCI_BEGINUNDOACTION, 0, 0);
	teco_interface_ssm(SCI_DELETERANGE, from, len);
	teco_interface_ssm(SCI_ENDUNDOACTION, 0, 0);
	teco_ring_dirtify();

	return TRUE;
}

/*$ K kill
 * [n]K -- Kill lines
 * -K
 * from,to K
 * [n]:K -> Success|Failure
 * from,to:K -> Success|Failure
 *
 * Deletes characters up to the beginning of the
 * line <n> lines after or before the current one.
 * If <n> is 0, \(lqK\(rq will delete up to the beginning
 * of the current line.
 * If <n> is omitted, the sign prefix will be implied.
 * So to delete the entire line regardless of the position
 * in it, one can use \(lq0KK\(rq.
 *
 * If the deletion is beyond the buffer's range, the command
 * will yield an error unless it has been colon-modified
 * so it returns a condition code.
 *
 * If two arguments <from> and <to> are available, the
 * command is synonymous to <from>,<to>D.
 */
void
teco_state_start_kill_lines(teco_machine_main_t *ctx, GError **error)
{
	teco_state_start_kill(ctx, "K", TRUE, error);
}

/*$ D delete
 * [n]D -- Delete characters
 * -D
 * from,to D
 * [n]:D -> Success|Failure
 * from,to:D -> Success|Failure
 *
 * If <n> is positive, the next <n> characters (up to and
 * character .+<n>) are deleted.
 * If <n> is negative, the previous <n> characters are
 * deleted.
 * If <n> is omitted, the sign prefix will be implied.
 *
 * If two arguments can be popped from the stack, the
 * command will delete the characters with absolute
 * position <from> up to <to> from the current buffer.
 *
 * If the character range to delete is beyond the buffer's
 * range, the command will yield an error unless it has
 * been colon-modified so it returns a condition code
 * instead.
 */
void
teco_state_start_delete_chars(teco_machine_main_t *ctx, GError **error)
{
	teco_state_start_kill(ctx, "D", FALSE, error);
}

/*$ ^Q lines2glyphs glyphs2lines
 * [n]^Q -> glyphs -- Convert between lines and glyph lengths or positions
 * [position]:^Q -> line
 *
 * Converts between line and glyph arguments.
 * It returns the number of glyphs between dot and the <n>-th next
 * line (or previous line if <n> is negative).
 * Consequently \(lq^QC\(rq is equivalent to \(lqL\(rq, but less efficient.
 *
 * If colon-modified, an absolute buffer position is converted to the line that
 * contains this position, beginning with 1.
 * Without arguments, \(lq:^Q\(rq returns the current line.
 */
/*
 * FIXME: Perhaps there should be a way to convert an absolute line to an
 * absolute position.
 */
void
teco_state_control_lines2glyphs(teco_machine_main_t *ctx, GError **error)
{
	if (!teco_expressions_eval(FALSE, error))
		return;

	if (teco_machine_main_eval_colon(ctx)) {
		gssize pos;

		if (!teco_expressions_args()) {
			pos = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);
		} else {
			teco_int_t v;

			if (!teco_expressions_pop_num_calc(&v, 0, error))
				return;

			pos = teco_interface_glyphs2bytes(v);
			if (pos < 0) {
				teco_error_range_set(error, "^Q");
				return;
			}
		}

		teco_expressions_push(teco_interface_ssm(SCI_LINEFROMPOSITION, pos, 0)+1);
	} else {
		teco_int_t v;

		if (!teco_expressions_pop_num_calc(&v, teco_num_sign, error))
			return;

		sptr_t pos = teco_interface_ssm(SCI_GETCURRENTPOS, 0, 0);
		sptr_t line = teco_interface_ssm(SCI_LINEFROMPOSITION, pos, 0) + v;

		if (!teco_validate_line(line)) {
			teco_error_range_set(error, "^Q");
			return;
		}

		sptr_t line_pos = teco_interface_ssm(SCI_POSITIONFROMLINE, line, 0);
		teco_expressions_push(teco_interface_bytes2glyphs(line_pos) - teco_interface_bytes2glyphs(pos));
	}
}
