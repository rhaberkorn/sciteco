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
#include <glib/gstdio.h>

#include "sciteco.h"
#include "memory.h"
#include "string-utils.h"
#include "interface.h"
#include "undo.h"
#include "expressions.h"
#include "qreg.h"
#include "ring.h"
#include "glob.h"
#include "error.h"
#include "core-commands.h"
#include "goto-commands.h"
#include "parser.h"

//#define DEBUG

GArray *teco_loop_stack;

static void __attribute__((constructor))
teco_loop_stack_init(void)
{
	teco_loop_stack = g_array_sized_new(FALSE, FALSE, sizeof(teco_loop_context_t), 1024);
}

TECO_DEFINE_ARRAY_UNDO_INSERT_VAL(teco_loop_stack, teco_loop_context_t);
TECO_DEFINE_ARRAY_UNDO_REMOVE_INDEX(teco_loop_stack);

static void TECO_DEBUG_CLEANUP
teco_loop_stack_cleanup(void)
{
	g_array_free(teco_loop_stack, TRUE);
}

gboolean
teco_machine_input(teco_machine_t *ctx, gunichar chr, GError **error)
{
	teco_state_t *next = ctx->current->input_cb(ctx, chr, error);
	if (!next)
		return FALSE;

	if (next != ctx->current) {
		if (ctx->must_undo)
			teco_undo_ptr(ctx->current);
		ctx->current = next;

		if (ctx->current->initial_cb && !ctx->current->initial_cb(ctx, error))
			return FALSE;
	}

	return TRUE;
}

gboolean
teco_state_end_of_macro(teco_machine_t *ctx, GError **error)
{
	g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
	                    "Unterminated command");
	return FALSE;
}

/**
 * Execute macro from current PC to stop position.
 *
 * Handles all expected exceptions and preparing them for stack frame insertion.
 *
 * @param ctx State machine.
 * @param macro The macro to execute.
 *   It does not have to be complete.
 *   It must consist only of validated UTF-8 sequences, though.
 * @param stop_pos Where to stop execution in bytes.
 * @param error Location to store error.
 * @return FALSE if an error occurred.
 */
gboolean
teco_machine_main_step(teco_machine_main_t *ctx, const gchar *macro, gsize stop_pos, GError **error)
{
	gsize last_pc = 0;

	while (ctx->macro_pc < stop_pos) {
		last_pc = ctx->macro_pc;

		if (G_UNLIKELY(teco_interface_is_interrupted())) {
			teco_error_interrupted_set(error);
			goto error_attach;
		}

		/*
		 * Most allocations are small or of limited size,
		 * so it is (almost) sufficient to check the memory limit regularily.
		 */
		if (!teco_memory_check(0, error))
			goto error_attach;

		/* UTF-8 sequences are already validated */
		gunichar chr = g_utf8_get_char(macro+ctx->macro_pc);

#ifdef DEBUG
		g_printf("EXEC(%d): input='%C' (U+%04" G_GINT32_MODIFIER "X), state=%p, mode=%d\n",
			 ctx->macro_pc, chr, chr, ctx->parent.current, ctx->mode);
#endif

		ctx->macro_pc = g_utf8_next_char(macro+ctx->macro_pc) - macro;

		if (!teco_machine_input(&ctx->parent, chr, error))
			goto error_attach;
	}

	/*
	 * Provide interactive feedback when the
	 * PC is at the end of the command line.
	 * This will actually be called in other situations,
	 * like at the end of macros but that does not hurt.
	 * It should perhaps be in teco_cmdline_insert(),
	 * but doing it here ensures that exceptions get
	 * normalized.
	 */
	if (ctx->parent.current->refresh_cb &&
	    !ctx->parent.current->refresh_cb(&ctx->parent, error))
		goto error_attach;

	return TRUE;

error_attach:
	g_assert(!error || *error != NULL);
	/*
	 * FIXME: Maybe this can be avoided altogether by passing in ctx->macro_pc
	 * from the callees?
	 */
	teco_error_set_coord(macro, last_pc);
	return FALSE;
}

gboolean
teco_execute_macro(const gchar *macro, gsize macro_len,
                   teco_qreg_table_t *qreg_table_locals, GError **error)
{
	const teco_string_t str = {(gchar *)macro, macro_len};

	if (!teco_string_validate_utf8(&str)) {
		g_set_error_literal(error, TECO_ERROR, TECO_ERROR_CODEPOINT,
		                    "Invalid UTF-8 byte sequence in macro");
		return FALSE;
	}

	/*
	 * This is not auto-cleaned up, so it can be initialized
	 * on demand.
	 */
	teco_qreg_table_t macro_locals;

	if (!qreg_table_locals)
		teco_qreg_table_init(&macro_locals, FALSE);

	guint parent_brace_level = teco_brace_level;

	g_auto(teco_machine_main_t) macro_machine;
	teco_machine_main_init(&macro_machine, qreg_table_locals ? : &macro_locals, FALSE);

	GError *tmp_error = NULL;

	if (!teco_machine_main_step(&macro_machine, macro, macro_len, &tmp_error)) {
		if (!g_error_matches(tmp_error, TECO_ERROR, TECO_ERROR_RETURN)) {
			/* passes ownership of tmp_error */
			g_propagate_error(error, tmp_error);
			goto error_cleanup;
		}
		g_error_free(tmp_error);

		/*
		 * Macro returned - handle like regular
		 * end of macro, even though some checks
		 * are unnecessary here.
		 * macro_pc will still point to the return PC.
		 */
		g_assert(macro_machine.parent.current == &teco_state_start);

		/*
		 * Discard all braces, except the current one.
		 */
		if (!teco_expressions_brace_return(parent_brace_level, teco_error_return_args, error))
			goto error_cleanup;

		/*
		 * Clean up the loop stack.
		 * We are allowed to return in loops.
		 * NOTE: This does not have to be undone.
		 */
		g_array_remove_range(teco_loop_stack, macro_machine.loop_stack_fp,
		                     teco_loop_stack->len - macro_machine.loop_stack_fp);
	}

	if (G_UNLIKELY(teco_loop_stack->len > macro_machine.loop_stack_fp)) {
		const teco_loop_context_t *ctx = &g_array_index(teco_loop_stack, teco_loop_context_t, teco_loop_stack->len-1);
		/* ctx->pc points to the character after the loop start command */
		teco_error_set_coord(macro, ctx->pc-1);

		g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
		                    "Unterminated loop");
		goto error_cleanup;
	}

	if (G_UNLIKELY(teco_goto_skip_label.len > 0)) {
		g_autofree gchar *label_printable = teco_string_echo(teco_goto_skip_label.data, teco_goto_skip_label.len);
		g_set_error(error, TECO_ERROR, TECO_ERROR_FAILED,
		            "Label \"%s\" not found", label_printable);
		goto error_attach;
	}

	/*
	 * Some states (esp. commands involving a
	 * "lookahead") are valid at the end of a macro.
	 */
	if (macro_machine.parent.current->end_of_macro_cb &&
	    !macro_machine.parent.current->end_of_macro_cb(&macro_machine.parent, error))
		goto error_attach;

	/*
	 * This handles the problem of Q-Registers
	 * local to the macro invocation being edited
	 * when the macro terminates without additional
	 * complexity.
	 * teco_qreg_table_empty() might leave the table
	 * half-empty, but it will eventually be completely
	 * cleared by teco_qreg_table_clear().
	 * This does not hurt since an error will rub out the
	 * macro invocation itself and macro_locals don't have
	 * to be preserved.
	 */
	if (!qreg_table_locals && !teco_qreg_table_empty(&macro_locals, error))
		goto error_attach;

	return TRUE;

error_attach:
	teco_error_set_coord(macro, macro_machine.macro_pc);
	/* fall through */
error_cleanup:
	if (!qreg_table_locals)
		teco_qreg_table_clear(&macro_locals);
	/* make sure teco_goto_skip_label will be NULL even in batch mode */
	teco_string_truncate(&teco_goto_skip_label, 0);
	return FALSE;
}

gboolean
teco_execute_file(const gchar *filename, teco_qreg_table_t *qreg_table_locals, GError **error)
{
	g_auto(teco_string_t) macro = {NULL, 0};
	if (!g_file_get_contents(filename, &macro.data, &macro.len, error))
		return FALSE;

	gchar *p;

	/* only when executing files, ignore Hash-Bang line */
	if (*macro.data == '#') {
		/*
		 * NOTE: We assume that a file starting with Hash does not contain
		 * a null-byte in its first line.
		 */
		p = strpbrk(macro.data, "\r\n");
		if (G_UNLIKELY(!p))
			/* empty script */
			return TRUE;
		p++;
	} else {
		p = macro.data;
	}

	if (!teco_execute_macro(p, macro.len - (p - macro.data),
	                        qreg_table_locals, error)) {
		/* correct error position for Hash-Bang line */
		teco_error_pos += p - macro.data;
		if (*macro.data == '#')
			teco_error_line++;
		teco_error_add_frame_file(filename);
		return FALSE;
	}

	return TRUE;
}

void
teco_machine_main_init(teco_machine_main_t *ctx, teco_qreg_table_t *qreg_table_locals,
                       gboolean must_undo)
{
	memset(ctx, 0, sizeof(*ctx));
	teco_machine_init(&ctx->parent, &teco_state_start, must_undo);
	ctx->loop_stack_fp = teco_loop_stack->len;
	teco_goto_table_init(&ctx->goto_table, must_undo);
	ctx->qreg_table_locals = qreg_table_locals;

	ctx->expectstring.nesting = 1;
	teco_machine_stringbuilding_init(&ctx->expectstring.machine, '\e', qreg_table_locals, must_undo);
}

gboolean
teco_machine_main_eval_colon(teco_machine_main_t *ctx)
{
	if (!ctx->modifier_colon)
		return FALSE;

	if (ctx->parent.must_undo)
		teco_undo_guint(ctx->__flags);
	ctx->modifier_colon = FALSE;
	return TRUE;
}

teco_state_t *
teco_machine_main_transition_input(teco_machine_main_t *ctx,
                                   teco_machine_main_transition_t *transitions,
                                   guint len, gunichar chr, GError **error)
{
	if (chr >= len || !transitions[chr].next) {
		teco_error_syntax_set(error, chr);
		return NULL;
	}

	if (ctx->mode == TECO_MODE_NORMAL && transitions[chr].transition_cb) {
		/*
		 * NOTE: We could also just let transition_cb return a boolean...
		 */
		GError *tmp_error = NULL;
		transitions[chr].transition_cb(ctx, &tmp_error);
		if (tmp_error) {
			g_propagate_error(error, tmp_error);
			return NULL;
		}
	}

	return transitions[chr].next;
}

void
teco_machine_main_clear(teco_machine_main_t *ctx)
{
	teco_goto_table_clear(&ctx->goto_table);
	teco_machine_stringbuilding_clear(&ctx->expectstring.machine);
}

/** Append string to result with case folding. */
static void
teco_machine_stringbuilding_append(teco_machine_stringbuilding_t *ctx, const gchar *str, gsize len)
{
	g_assert(ctx->result != NULL);

	switch (ctx->mode) {
	case TECO_STRINGBUILDING_MODE_NORMAL:
		teco_string_append(ctx->result, str, len);
		break;
	case TECO_STRINGBUILDING_MODE_UPPER: {
		g_autofree gchar *folded = ctx->codepage == SC_CP_UTF8
						? g_utf8_strup(str, len) : g_ascii_strup(str, len);
		teco_string_append(ctx->result, folded, strlen(folded));
		break;
	}
	case TECO_STRINGBUILDING_MODE_LOWER: {
		g_autofree gchar *folded = ctx->codepage == SC_CP_UTF8
						? g_utf8_strdown(str, len) : g_ascii_strdown(str, len);
		teco_string_append(ctx->result, folded, strlen(folded));
		break;
	}
	}
}

/*
 * FIXME: All teco_state_stringbuilding_* states could be static?
 */
static teco_state_t *teco_state_stringbuilding_ctl_input(teco_machine_stringbuilding_t *ctx,
                                                         gunichar chr, GError **error);
TECO_DECLARE_STATE(teco_state_stringbuilding_ctl);

static teco_state_t *teco_state_stringbuilding_escaped_input(teco_machine_stringbuilding_t *ctx,
                                                             gunichar chr, GError **error);
TECO_DECLARE_STATE(teco_state_stringbuilding_escaped);

TECO_DECLARE_STATE(teco_state_stringbuilding_lower);
TECO_DECLARE_STATE(teco_state_stringbuilding_upper);

TECO_DECLARE_STATE(teco_state_stringbuilding_ctle);
TECO_DECLARE_STATE(teco_state_stringbuilding_ctle_num);
TECO_DECLARE_STATE(teco_state_stringbuilding_ctle_u);
TECO_DECLARE_STATE(teco_state_stringbuilding_ctle_q);
TECO_DECLARE_STATE(teco_state_stringbuilding_ctle_quote);
TECO_DECLARE_STATE(teco_state_stringbuilding_ctle_n);

static teco_state_t *
teco_state_stringbuilding_start_input(teco_machine_stringbuilding_t *ctx, gunichar chr, GError **error)
{
	switch (chr) {
	case '^':
		return &teco_state_stringbuilding_ctl;
	case TECO_CTL_KEY('^'):
		/*
		 * Ctrl+^ is inserted verbatim as code 30.
		 * Otherwise it would expand to a single caret
		 * just like caret+caret (^^).
		 */
		break;
	default:
		if (TECO_IS_CTL(chr))
			return teco_state_stringbuilding_ctl_input(ctx, TECO_CTL_ECHO(chr), error);
	}

	return teco_state_stringbuilding_escaped_input(ctx, chr, error);
}

/* in cmdline.c */
gboolean teco_state_stringbuilding_start_process_edit_cmd(teco_machine_stringbuilding_t *ctx, teco_machine_t *parent_ctx,
                                                          gunichar key, GError **error);

TECO_DEFINE_STATE(teco_state_stringbuilding_start,
		.is_start = TRUE,
		.process_edit_cmd_cb = (teco_state_process_edit_cmd_cb_t)
		                       teco_state_stringbuilding_start_process_edit_cmd
);

static teco_state_t *
teco_state_stringbuilding_ctl_input(teco_machine_stringbuilding_t *ctx, gunichar chr, GError **error)
{
	chr = teco_ascii_toupper(chr);

	switch (chr) {
	case '^':
		/*
		 * Double-caret expands to a single caret.
		 * Ctrl+^ (30) is handled separately and inserts code 30.
		 * The special handling of the double-caret should perhaps
		 * be abolished altogether.
		 */
		break;
	case 'Q':
	case 'R': return &teco_state_stringbuilding_escaped;
	case 'V': return &teco_state_stringbuilding_lower;
	case 'W': return &teco_state_stringbuilding_upper;
	case 'E': return &teco_state_stringbuilding_ctle;
	default:
		chr = TECO_CTL_KEY(chr);
	}

	/*
	 * Source code is always in UTF-8, so it does not
	 * make sense to handle ctx->codepage != SC_CP_UTF8
	 * separately.
	 */
	if (ctx->result)
		teco_string_append_wc(ctx->result, chr);
	return &teco_state_stringbuilding_start;
}

TECO_DEFINE_STATE_CASEINSENSITIVE(teco_state_stringbuilding_ctl);

static teco_state_t *
teco_state_stringbuilding_escaped_input(teco_machine_stringbuilding_t *ctx, gunichar chr, GError **error)
{
	if (!ctx->result)
		/* parse-only mode */
		return &teco_state_stringbuilding_start;

	/*
	 * The subtle difference between UTF-8 and single-byte targets
	 * is that we don't try to casefold non-ANSI characters in single-byte mode.
	 */
	switch (ctx->mode) {
	case TECO_STRINGBUILDING_MODE_NORMAL:
		break;
	case TECO_STRINGBUILDING_MODE_UPPER:
		chr = ctx->codepage == SC_CP_UTF8 || chr < 0x80
					? g_unichar_toupper(chr) : chr;
		break;
	case TECO_STRINGBUILDING_MODE_LOWER:
		chr = ctx->codepage == SC_CP_UTF8 || chr < 0x80
					? g_unichar_tolower(chr) : chr;
		break;
	}

	teco_string_append_wc(ctx->result, chr);
	return &teco_state_stringbuilding_start;
}

/* in cmdline.c */
gboolean teco_state_stringbuilding_escaped_process_edit_cmd(teco_machine_stringbuilding_t *ctx, teco_machine_t *parent_ctx,
                                                            gunichar key, GError **error);

TECO_DEFINE_STATE(teco_state_stringbuilding_escaped,
	.process_edit_cmd_cb = (teco_state_process_edit_cmd_cb_t)
	                       teco_state_stringbuilding_escaped_process_edit_cmd
);

static teco_state_t *
teco_state_stringbuilding_lower_ctl_input(teco_machine_stringbuilding_t *ctx, gunichar chr, GError **error)
{
	if (!ctx->result)
		/* parse-only mode */
		return &teco_state_stringbuilding_start;

	chr = teco_ascii_toupper(chr);

	if (chr == 'V') {
		if (ctx->parent.must_undo)
			teco_undo_guint(ctx->mode);
		ctx->mode = TECO_STRINGBUILDING_MODE_LOWER;
	} else {
		/* control keys cannot be case folded */
		teco_string_append_wc(ctx->result, TECO_CTL_KEY(chr));
	}

	return &teco_state_stringbuilding_start;
}

TECO_DEFINE_STATE_CASEINSENSITIVE(teco_state_stringbuilding_lower_ctl);

static teco_state_t *
teco_state_stringbuilding_lower_input(teco_machine_stringbuilding_t *ctx, gunichar chr, GError **error)
{
	if (chr == '^')
		return &teco_state_stringbuilding_lower_ctl;
	if (TECO_IS_CTL(chr))
		return teco_state_stringbuilding_lower_ctl_input(ctx, TECO_CTL_ECHO(chr), error);

	if (ctx->result) {
		chr = ctx->codepage == SC_CP_UTF8 || chr < 0x80
					? g_unichar_tolower(chr) : chr;
		teco_string_append_wc(ctx->result, chr);
	}
	return &teco_state_stringbuilding_start;
}

TECO_DEFINE_STATE(teco_state_stringbuilding_lower);

static teco_state_t *
teco_state_stringbuilding_upper_ctl_input(teco_machine_stringbuilding_t *ctx, gunichar chr, GError **error)
{
	if (!ctx->result)
		/* parse-only mode */
		return &teco_state_stringbuilding_start;

	chr = teco_ascii_toupper(chr);

	if (chr == 'W') {
		if (ctx->parent.must_undo)
			teco_undo_guint(ctx->mode);
		ctx->mode = TECO_STRINGBUILDING_MODE_UPPER;
	} else {
		/* control keys cannot be case folded */
		teco_string_append_wc(ctx->result, TECO_CTL_KEY(chr));
	}

	return &teco_state_stringbuilding_start;
}

TECO_DEFINE_STATE_CASEINSENSITIVE(teco_state_stringbuilding_upper_ctl);

static teco_state_t *
teco_state_stringbuilding_upper_input(teco_machine_stringbuilding_t *ctx, gunichar chr, GError **error)
{
	if (chr == '^')
		return &teco_state_stringbuilding_upper_ctl;
	if (TECO_IS_CTL(chr))
		return teco_state_stringbuilding_upper_ctl_input(ctx, TECO_CTL_ECHO(chr), error);

	if (ctx->result) {
		chr = ctx->codepage == SC_CP_UTF8 || chr < 0x80
					? g_unichar_toupper(chr) : chr;
		teco_string_append_wc(ctx->result, chr);
	}
	return &teco_state_stringbuilding_start;
}

TECO_DEFINE_STATE(teco_state_stringbuilding_upper);

static teco_state_t *
teco_state_stringbuilding_ctle_input(teco_machine_stringbuilding_t *ctx, gunichar chr, GError **error)
{
	teco_state_t *next;

	switch (teco_ascii_toupper(chr)) {
	case '\\': next = &teco_state_stringbuilding_ctle_num; break;
	case 'U':  next = &teco_state_stringbuilding_ctle_u; break;
	case 'Q':  next = &teco_state_stringbuilding_ctle_q; break;
	case '@':  next = &teco_state_stringbuilding_ctle_quote; break;
	case 'N':  next = &teco_state_stringbuilding_ctle_n; break;
	default:
		if (ctx->result) {
			/* also makes sure that search patterns can start with ^E */
			gchar buf[1+6] = {TECO_CTL_KEY('E')};
			gsize len = g_unichar_to_utf8(chr, buf+1);
			teco_machine_stringbuilding_append(ctx, buf, 1+len);
		}
		return &teco_state_stringbuilding_start;
	}

	if (ctx->machine_qregspec)
		teco_machine_qregspec_reset(ctx->machine_qregspec);
	else
		ctx->machine_qregspec = teco_machine_qregspec_new(TECO_QREG_REQUIRED,
		                                                  ctx->qreg_table_locals,
		                                                  ctx->parent.must_undo);
	return next;
}

TECO_DEFINE_STATE_CASEINSENSITIVE(teco_state_stringbuilding_ctle);

/* in cmdline.c */
gboolean teco_state_stringbuilding_qreg_process_edit_cmd(teco_machine_stringbuilding_t *ctx, teco_machine_t *parent_ctx,
                                                         gunichar chr, GError **error);

/**
 * @interface TECO_DEFINE_STATE_STRINGBUILDING_QREG
 * @implements TECO_DEFINE_STATE
 * @ingroup states
 */
#define TECO_DEFINE_STATE_STRINGBUILDING_QREG(NAME, ...) \
	TECO_DEFINE_STATE(NAME, \
		.process_edit_cmd_cb = (teco_state_process_edit_cmd_cb_t) \
		                       teco_state_stringbuilding_qreg_process_edit_cmd, \
		##__VA_ARGS__ \
	)

static teco_state_t *
teco_state_stringbuilding_ctle_num_input(teco_machine_stringbuilding_t *ctx, gunichar chr, GError **error)
{
	teco_qreg_t *qreg;

	switch (teco_machine_qregspec_input(ctx->machine_qregspec, chr,
	                                    ctx->result ? &qreg : NULL, NULL, error)) {
	case TECO_MACHINE_QREGSPEC_ERROR:
		return NULL;
	case TECO_MACHINE_QREGSPEC_MORE:
		return &teco_state_stringbuilding_ctle_num;
	case TECO_MACHINE_QREGSPEC_DONE:
		break;
	}

	if (!ctx->result)
		/* parse-only mode */
		return &teco_state_stringbuilding_start;

	teco_int_t value;
	if (!qreg->vtable->get_integer(qreg, &value, error))
		return NULL;

	/*
	 * NOTE: Numbers can always be safely formatted as null-terminated strings.
	 */
	gchar buffer[TECO_EXPRESSIONS_FORMAT_LEN];
	const gchar *num = teco_expressions_format(buffer, value);
	teco_machine_stringbuilding_append(ctx, num, strlen(num));

	return &teco_state_stringbuilding_start;
}

TECO_DEFINE_STATE_STRINGBUILDING_QREG(teco_state_stringbuilding_ctle_num);

static teco_state_t *
teco_state_stringbuilding_ctle_u_input(teco_machine_stringbuilding_t *ctx, gunichar chr, GError **error)
{
	teco_qreg_t *qreg;

	switch (teco_machine_qregspec_input(ctx->machine_qregspec, chr,
	                                    ctx->result ? &qreg : NULL, NULL, error)) {
	case TECO_MACHINE_QREGSPEC_ERROR:
		return NULL;
	case TECO_MACHINE_QREGSPEC_MORE:
		return &teco_state_stringbuilding_ctle_u;
	case TECO_MACHINE_QREGSPEC_DONE:
		break;
	}

	if (!ctx->result)
		/* parse-only mode */
		return &teco_state_stringbuilding_start;

	teco_int_t value;
	if (!qreg->vtable->get_integer(qreg, &value, error))
		return NULL;

	if (ctx->codepage == SC_CP_UTF8) {
		if (value < 0 || !g_unichar_validate(value))
			goto error_codepoint;
		switch (ctx->mode) {
		case TECO_STRINGBUILDING_MODE_NORMAL:
			break;
		case TECO_STRINGBUILDING_MODE_UPPER:
			value = g_unichar_toupper(value);
			break;
		case TECO_STRINGBUILDING_MODE_LOWER:
			value = g_unichar_tolower(value);
			break;
		}
		teco_string_append_wc(ctx->result, value);
	} else {
		if (value < 0 || value > 0xFF)
			goto error_codepoint;
		switch (ctx->mode) {
		case TECO_STRINGBUILDING_MODE_NORMAL:
			break;
		case TECO_STRINGBUILDING_MODE_UPPER:
			value = g_ascii_toupper(value);
			break;
		case TECO_STRINGBUILDING_MODE_LOWER:
			value = g_ascii_tolower(value);
			break;
		}
		teco_string_append_c(ctx->result, value);
	}

	return &teco_state_stringbuilding_start;

error_codepoint: {
	g_autofree gchar *name_printable = teco_string_echo(qreg->head.name.data, qreg->head.name.len);
	g_set_error(error, TECO_ERROR, TECO_ERROR_CODEPOINT,
	            "Q-Register \"%s\" does not contain a valid codepoint", name_printable);
	return NULL;
}
}

TECO_DEFINE_STATE_STRINGBUILDING_QREG(teco_state_stringbuilding_ctle_u);

static teco_state_t *
teco_state_stringbuilding_ctle_q_input(teco_machine_stringbuilding_t *ctx, gunichar chr, GError **error)
{
	teco_qreg_t *qreg;

	switch (teco_machine_qregspec_input(ctx->machine_qregspec, chr,
	                                    ctx->result ? &qreg : NULL, NULL, error)) {
	case TECO_MACHINE_QREGSPEC_ERROR:
		return NULL;
	case TECO_MACHINE_QREGSPEC_MORE:
		return &teco_state_stringbuilding_ctle_q;
	case TECO_MACHINE_QREGSPEC_DONE:
		break;
	}

	if (!ctx->result)
		/* parse-only mode */
		return &teco_state_stringbuilding_start;

	g_auto(teco_string_t) str = {NULL, 0};
	if (!qreg->vtable->get_string(qreg, &str.data, &str.len, NULL, error))
		return NULL;
	teco_machine_stringbuilding_append(ctx, str.data, str.len);
	return &teco_state_stringbuilding_start;
}

TECO_DEFINE_STATE_STRINGBUILDING_QREG(teco_state_stringbuilding_ctle_q);

static teco_state_t *
teco_state_stringbuilding_ctle_quote_input(teco_machine_stringbuilding_t *ctx, gunichar chr, GError **error)
{
	teco_qreg_t *qreg;
	teco_qreg_table_t *table;

	switch (teco_machine_qregspec_input(ctx->machine_qregspec, chr,
	                                    ctx->result ? &qreg : NULL, &table, error)) {
	case TECO_MACHINE_QREGSPEC_ERROR:
		return NULL;
	case TECO_MACHINE_QREGSPEC_MORE:
		return &teco_state_stringbuilding_ctle_quote;
	case TECO_MACHINE_QREGSPEC_DONE:
		break;
	}

	if (!ctx->result)
		/* parse-only mode */
		return &teco_state_stringbuilding_start;

	g_auto(teco_string_t) str = {NULL, 0};
	if (!qreg->vtable->get_string(qreg, &str.data, &str.len, NULL, error))
		return NULL;
	/*
	 * NOTE: g_shell_quote() expects a null-terminated string, so it is
	 * important to check that there are no embedded nulls.
	 * The restriction itself is probably valid since null-bytes are not allowed
	 * in command line arguments anyway.
	 * Otherwise, we'd have to implement our own POSIX shell escape function.
	 */
	if (teco_string_contains(&str, '\0')) {
		teco_error_qregcontainsnull_set(error, qreg->head.name.data, qreg->head.name.len,
		                                table != &teco_qreg_table_globals);
		return NULL;
	}
	g_autofree gchar *str_quoted = g_shell_quote(str.data ? : "");
	teco_machine_stringbuilding_append(ctx, str_quoted, strlen(str_quoted));

	return &teco_state_stringbuilding_start;
}

TECO_DEFINE_STATE_STRINGBUILDING_QREG(teco_state_stringbuilding_ctle_quote);

static teco_state_t *
teco_state_stringbuilding_ctle_n_input(teco_machine_stringbuilding_t *ctx, gunichar chr, GError **error)
{
	teco_qreg_t *qreg;
	teco_qreg_table_t *table;

	switch (teco_machine_qregspec_input(ctx->machine_qregspec, chr,
	                                    ctx->result ? &qreg : NULL, &table, error)) {
	case TECO_MACHINE_QREGSPEC_ERROR:
		return NULL;
	case TECO_MACHINE_QREGSPEC_MORE:
		return &teco_state_stringbuilding_ctle_n;
	case TECO_MACHINE_QREGSPEC_DONE:
		break;
	}

	if (!ctx->result)
		/* parse-only mode */
		return &teco_state_stringbuilding_start;

	g_auto(teco_string_t) str = {NULL, 0};
	if (!qreg->vtable->get_string(qreg, &str.data, &str.len, NULL, error))
		return NULL;
	if (teco_string_contains(&str, '\0')) {
		teco_error_qregcontainsnull_set(error, qreg->head.name.data, qreg->head.name.len,
		                                table != &teco_qreg_table_globals);
		return NULL;
	}

	g_autofree gchar *str_escaped = teco_globber_escape_pattern(str.data);
	teco_machine_stringbuilding_append(ctx, str_escaped, strlen(str_escaped));

	return &teco_state_stringbuilding_start;
}

TECO_DEFINE_STATE_STRINGBUILDING_QREG(teco_state_stringbuilding_ctle_n);

void
teco_machine_stringbuilding_init(teco_machine_stringbuilding_t *ctx, gunichar escape_char,
                                 teco_qreg_table_t *locals, gboolean must_undo)
{
	memset(ctx, 0, sizeof(*ctx));
	teco_machine_init(&ctx->parent, &teco_state_stringbuilding_start, must_undo);
	ctx->escape_char = escape_char;
	ctx->qreg_table_locals = locals;
	ctx->codepage = teco_default_codepage();
}

void
teco_machine_stringbuilding_reset(teco_machine_stringbuilding_t *ctx)
{
	teco_machine_reset(&ctx->parent, &teco_state_stringbuilding_start);
	if (ctx->machine_qregspec)
		teco_machine_qregspec_reset(ctx->machine_qregspec);
	if (ctx->parent.must_undo)
		teco_undo_guint(ctx->mode);
	ctx->mode = TECO_STRINGBUILDING_MODE_NORMAL;
}

/*
 * If we case folded only ANSI characters as in teco_ascii_toupper(),
 * this could be simplified.
 */
void
teco_machine_stringbuilding_escape(teco_machine_stringbuilding_t *ctx, const gchar *str, gsize len,
                                   teco_string_t *target)
{
	target->data = g_malloc(len*2+1);
	target->len = 0;

	for (guint i = 0; i < len; ) {
		gunichar chr = g_utf8_get_char(str+i);

		if (g_unichar_toupper(chr) == ctx->escape_char ||
		    (ctx->escape_char == '[' && chr == ']') ||
		    (ctx->escape_char == '{' && chr == '}'))
			target->data[target->len++] = TECO_CTL_KEY('Q');

		gsize lenc = g_utf8_next_char(str+i) - (str+i);
		memcpy(target->data+target->len, str+i, lenc);
		target->len += lenc;
		i += lenc;
	}

	target->data[target->len] = '\0';
}

void
teco_machine_stringbuilding_clear(teco_machine_stringbuilding_t *ctx)
{
	if (ctx->machine_qregspec)
		teco_machine_qregspec_free(ctx->machine_qregspec);
}

gboolean
teco_state_expectstring_initial(teco_machine_main_t *ctx, GError **error)
{
	if (ctx->mode == TECO_MODE_NORMAL)
		teco_machine_stringbuilding_set_codepage(&ctx->expectstring.machine,
		                                         teco_default_codepage());
	return TRUE;
}

teco_state_t *
teco_state_expectstring_input(teco_machine_main_t *ctx, gunichar chr, GError **error)
{
	teco_state_t *current = ctx->parent.current;

	/*
	 * String termination handling
	 */
	if (ctx->modifier_at) {
		if (current->expectstring.last) {
			if (ctx->parent.must_undo)
				teco_undo_guint(ctx->__flags);
			ctx->modifier_at = FALSE;
		}

		/*
		 * FIXME: Exclude setting at least whitespace characters as the
		 * new string escape character to avoid accidental errors?
		 *
		 * FIXME: Should we perhaps restrict case folding escape characters
		 * to the ANSI range (teco_ascii_toupper())?
		 * This would be faster than case folding each and every character
		 * of a string argument to check against the escape char.
		 */
		switch (ctx->expectstring.machine.escape_char) {
		case '\e':
		case '{':
			if (ctx->parent.must_undo)
				teco_undo_gunichar(ctx->expectstring.machine.escape_char);
			ctx->expectstring.machine.escape_char = g_unichar_toupper(chr);
			return current;
		}
	}

	/*
	 * This makes sure that escape characters (or braces) within string-building
	 * constructs and Q-Register specifications do not have to be escaped.
	 * This makes also sure that string terminators can be escaped via ^Q/^R.
	 */
	if (ctx->expectstring.machine.parent.current->is_start) {
		if (ctx->expectstring.machine.escape_char == '{') {
			switch (chr) {
			case '{':
				if (ctx->parent.must_undo)
					teco_undo_gint(ctx->expectstring.nesting);
				ctx->expectstring.nesting++;
				break;
			case '}':
				if (ctx->parent.must_undo)
					teco_undo_gint(ctx->expectstring.nesting);
				ctx->expectstring.nesting--;
				break;
			}
		} else if (g_unichar_toupper(chr) == ctx->expectstring.machine.escape_char) {
			if (ctx->parent.must_undo)
				teco_undo_gint(ctx->expectstring.nesting);
			ctx->expectstring.nesting--;
		}
	}

	if (!ctx->expectstring.nesting) {
		/*
		 * Call process_cb() even if interactive feedback
		 * has not been requested using refresh_cb().
		 * This is necessary since commands are either
		 * written for interactive execution or not,
		 * so they may do their main activity in process_cb().
		 */
		if (ctx->expectstring.insert_len && current->expectstring.process_cb &&
		    !current->expectstring.process_cb(ctx, &ctx->expectstring.string,
		                                      ctx->expectstring.insert_len, error))
			return NULL;

		teco_state_t *next = current->expectstring.done_cb(ctx, &ctx->expectstring.string, error);

		if (ctx->parent.must_undo)
			teco_undo_string_own(ctx->expectstring.string);
		else
			teco_string_clear(&ctx->expectstring.string);
		memset(&ctx->expectstring.string, 0, sizeof(ctx->expectstring.string));

		if (current->expectstring.last) {
			if (ctx->parent.must_undo)
				teco_undo_gunichar(ctx->expectstring.machine.escape_char);
			ctx->expectstring.machine.escape_char = '\e';
		}
		ctx->expectstring.nesting = 1;

		if (current->expectstring.string_building)
			teco_machine_stringbuilding_reset(&ctx->expectstring.machine);

		if (ctx->parent.must_undo)
			teco_undo_gsize(ctx->expectstring.insert_len);
		ctx->expectstring.insert_len = 0;
		return next;
	}

	/*
	 * NOTE: Since we only ever append to `string`, this is more efficient
	 * than teco_undo_string(ctx->expectstring.string).
	 */
	if (ctx->mode == TECO_MODE_NORMAL && ctx->parent.must_undo)
		undo__teco_string_truncate(&ctx->expectstring.string, ctx->expectstring.string.len);

	/*
	 * String building characters and string argument accumulation.
	 */
	gsize old_len = ctx->expectstring.string.len;
	if (current->expectstring.string_building) {
		teco_string_t *str = ctx->mode == TECO_MODE_NORMAL
						? &ctx->expectstring.string : NULL;
		if (!teco_machine_stringbuilding_input(&ctx->expectstring.machine, chr, str, error))
			return NULL;
	} else if (ctx->mode == TECO_MODE_NORMAL) {
		teco_string_append_wc(&ctx->expectstring.string, chr);
	}

	/*
	 * NOTE: insert_len is always 0 after key presses in interactive mode,
	 * so we wouldn't have to undo it.
	 * But there is one exception: When interrupting a loop including a string
	 * argument (e.g. <I...$>), insert_len could end up != 0 which would consequently
	 * crash once you change the string argument.
	 * The only way to avoid repeated undo tokens might be adding an initial() callback
	 * that sets it to 0 on undo. But that is very tricky.
	 */
	if (ctx->parent.must_undo)
		teco_undo_gsize(ctx->expectstring.insert_len);
	ctx->expectstring.insert_len += ctx->expectstring.string.len - old_len;

	return current;
}

gboolean
teco_state_expectstring_refresh(teco_machine_main_t *ctx, GError **error)
{
	teco_state_t *current = ctx->parent.current;

	/* never calls process_cb() in parse-only mode */
	if (ctx->expectstring.insert_len && current->expectstring.process_cb &&
	    !current->expectstring.process_cb(ctx, &ctx->expectstring.string,
	                                      ctx->expectstring.insert_len, error))
		return FALSE;

	if (ctx->parent.must_undo)
		teco_undo_gsize(ctx->expectstring.insert_len);
	ctx->expectstring.insert_len = 0;

	return TRUE;
}

gboolean
teco_state_expectfile_process(teco_machine_main_t *ctx, const teco_string_t *str,
                              gsize new_chars, GError **error)
{
	g_assert(str->data != NULL);

	/*
	 * Null-chars must not occur in filename/path strings and at some point
	 * teco_string_t has to be converted to a null-terminated C string
	 * as all the glib filename functions rely on null-terminated strings.
	 * Doing it here ensures that teco_file_expand_path() can be safely called
	 * from the done_cb().
	 */
	if (memchr(str->data + str->len - new_chars, '\0', new_chars)) {
		g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
		                    "Null-character not allowed in filenames");
		return FALSE;
	}

	return TRUE;
}
