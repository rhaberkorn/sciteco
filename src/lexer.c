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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include <glib.h>

#include "sciteco.h"
#include "view.h"
#include "parser.h"
#include "lexer.h"

static teco_style_t
teco_lexer_getstyle(teco_view_t *view, teco_machine_main_t *machine,
                    gunichar chr)
{
	teco_style_t style = machine->parent.current->style;

	/*
	 * FIXME: At least this special workaround for numbers might be
	 * unnecessary once we get a special parser state for parsing numbers.
	 *
	 * FIXME: What about ^* and ^/?
	 * They are currently highlighted as commands.
	 */
	if (machine->parent.current->keymacro_mask & TECO_KEYMACRO_MASK_START &&
	    chr <= 0xFF) {
		if (g_ascii_isdigit(chr))
			style = SCE_SCITECO_NUMBER;
		else if (strchr("+-*/#&", chr))
			style = SCE_SCITECO_OPERATOR;
	}

	/*
	 * FIXME: Perhaps as an optional lexer property, we should support
	 * styling commands with SCE_SCITECO_DEFAULT or SCE_SCITECO_COMMAND
	 * in alternating order, so you can discern chains of commands.
	 */
	if (!teco_machine_input(&machine->parent, chr, NULL)) {
		/*
		 * Probably a syntax error, so the erroneous symbol
		 * is highlighted and we reset the parser's state machine.
		 *
		 * FIXME: Perhaps we should simply reset the state to teco_state_start?
		 */
		gsize macro_pc = machine->macro_pc;
		teco_machine_main_clear(machine);
		teco_machine_main_init(machine, NULL, FALSE);
		machine->mode = TECO_MODE_LEXING;
		machine->macro_pc = macro_pc;

		return SCE_SCITECO_INVALID;
	}

	/*
	 * Don't highlight the leading `!` in comments as SCE_SCITECO_COMMAND.
	 * True comments also begin with `!`, so make sure they are highlighted
	 * already from the second character.
	 * This is then extended back by one character in teco_lexer_step().
	 */
	switch (machine->parent.current->style) {
	case SCE_SCITECO_COMMENT:
	case SCE_SCITECO_LABEL:
		return machine->parent.current->style;
	default:
		break;
	}

	return style;
}

static void
teco_lexer_step(teco_view_t *view, teco_machine_main_t *machine,
                teco_machine_main_t *macrodef_machine,
                const gchar *macro, gsize start, gsize max_len,
                guint *cur_line, guint *cur_col, gint *safe_col)
{
	if (*cur_line == 0 && *cur_col == 0 && *macro == '#') {
		/* hash-bang line */
		machine->macro_pc = teco_view_ssm(view, SCI_POSITIONFROMLINE, 1, 0);
		teco_view_ssm(view, SCI_STARTSTYLING, 0, 0);
		teco_view_ssm(view, SCI_SETSTYLING, machine->macro_pc, SCE_SCITECO_COMMENT);
		teco_view_ssm(view, SCI_SETLINESTATE, 0, -1);
		(*cur_line)++;
		*safe_col = 0;
		return;
	}

	gssize old_pc = machine->macro_pc;

	teco_style_t style = SCE_SCITECO_DEFAULT;

	/*
	 * g_utf8_get_char_validated() sometimes(?) returns -2 for "\0".
	 */
	gint32 chr = macro[machine->macro_pc]
			? g_utf8_get_char_validated(macro+machine->macro_pc,
			                            max_len-machine->macro_pc) : 0;
	if (chr < 0) {
		/*
		 * Invalid UTF-8 byte sequence:
		 * A source file could contain all sorts of data garbage or
		 * you could manually M[lexer.set.sciteco] on an ANSI-encoded file.
		 */
		machine->macro_pc++;
		style = SCE_SCITECO_INVALID;
	} else {
		machine->macro_pc = g_utf8_next_char(macro+machine->macro_pc) - macro;

		gunichar escape_char = machine->expectstring.machine.escape_char;
		style = teco_lexer_getstyle(view, machine, chr);

		/*
		 * Optionally style @^Uq{ ... } contents like macro definitions.
		 * The curly braces will be styled like regular commands.
		 *
		 * FIXME: This will not work with nested macro definitions.
		 * FIXME: This cannot currently be disabled since SCI_SETPROPERTY
		 * cannot be accessed with ES.
		 * We could only map it to an ED flag.
		 */
		if ((escape_char == '{' || machine->expectstring.machine.escape_char == '{') &&
		    teco_view_ssm(view, SCI_GETPROPERTYINT, (uptr_t)"lexer.sciteco.macrodef", TRUE))
			style = teco_lexer_getstyle(view, macrodef_machine, chr);
	}

	*cur_col += machine->macro_pc - old_pc;

	/*
	 * True comments begin with `!*` or `!!`, but only the second character gets
	 * the correct style by default, so we extend it backwards.
	 */
	if (style == SCE_SCITECO_COMMENT)
		old_pc--;

	teco_view_ssm(view, SCI_STARTSTYLING, start+old_pc, 0);
	teco_view_ssm(view, SCI_SETSTYLING, machine->macro_pc-old_pc, style);

	if (chr == '\n') {
		/* update line state to the last column with a clean start state */
		teco_view_ssm(view, SCI_SETLINESTATE, *cur_line, *safe_col);
		(*cur_line)++;
		*cur_col = 0;
		*safe_col = -1; /* no clean state by default */
	}

	if (style != SCE_SCITECO_INVALID &&
	    machine->parent.current->keymacro_mask & TECO_KEYMACRO_MASK_START &&
	    !machine->modifier_at)
		/* clean parser state */
		*safe_col = *cur_col;
}

/**
 * Style SciTECO source code, i.e. perform syntax highlighting
 * for the SciTECO language.
 *
 * @para view The Scintilla view to operate on.
 * @para end The position in bytes where to stop styling.
 */
void
teco_lexer_style(teco_view_t *view, gsize end)
{
	/* should always be TRUE */
	gboolean old_undo_enabled = teco_undo_enabled;
	teco_undo_enabled = FALSE;

	gsize start = teco_view_ssm(view, SCI_GETENDSTYLED, 0, 0);
	guint start_line = teco_view_ssm(view, SCI_LINEFROMPOSITION, start, 0);
	gint start_col = 0;

	/*
	 * The line state stores the laster character (column) in bytes,
	 * that starts from a fresh parser state.
	 * It's -1 if the line does not have a clean parser state.
	 * Therefore we search for the first line before `start` that has a
	 * known clean parser state.
	 */
	if (start_line > 0) {
		do
			start_line--;
		while ((start_col = teco_view_ssm(view, SCI_GETLINESTATE, start_line, 0)) < 0 &&
		       start_line > 0);
		start_col = MAX(start_col, 0);
	}
	start = teco_view_ssm(view, SCI_POSITIONFROMLINE, start_line, 0) + start_col;
	g_assert(end > start);

	g_auto(teco_machine_main_t) machine;
	teco_machine_main_init(&machine, NULL, FALSE);
	machine.mode = TECO_MODE_LEXING;

	/* for lexing the contents of @^Uq{...} */
	g_auto(teco_machine_main_t) macrodef_machine;
	teco_machine_main_init(&macrodef_machine, NULL, FALSE);
	macrodef_machine.mode = TECO_MODE_LEXING;

	g_assert(start_col >= 0);
	guint col = start_col;

	/*
	 * NOTE: We could have also used teco_view_get_character(),
	 * but this will use much less Scintilla messages without
	 * removing dot.
	 */
	const gchar *macro;
	sptr_t gap = teco_view_ssm(view, SCI_GETGAPPOSITION, 0, 0);
	if (start < gap && gap < end) {
		macro = (const gchar *)teco_view_ssm(view, SCI_GETRANGEPOINTER, start, gap);
		while (machine.macro_pc < gap-start)
			teco_lexer_step(view, &machine, &macrodef_machine,
			                macro, start, gap-start,
			                &start_line, &col, &start_col);
		/*
		 * This might have lexed more than gap-start bytes
		 * (e.g. a hash-bang line)
		 */
		start += machine.macro_pc;
	}

	macro = (const gchar *)teco_view_ssm(view, SCI_GETRANGEPOINTER, start, end-start);
	machine.macro_pc = 0;
	while (machine.macro_pc < end-start)
		teco_lexer_step(view, &machine, &macrodef_machine,
		                macro, start, end-start,
		                &start_line, &col, &start_col);

	/* set line state on the very last line */
	teco_view_ssm(view, SCI_SETLINESTATE, start_line, start_col);

	teco_undo_enabled = old_undo_enabled;
}
