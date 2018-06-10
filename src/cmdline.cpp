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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif

#include <string.h>
#include <signal.h>

#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>

#include "sciteco.h"
#include "string-utils.h"
#include "interface.h"
#include "expressions.h"
#include "parser.h"
#include "qregisters.h"
#include "ring.h"
#include "ioview.h"
#include "goto.h"
#include "help.h"
#include "undo.h"
#include "symbols.h"
#include "spawn.h"
#include "glob.h"
#include "error.h"
#include "cmdline.h"

extern "C" {
#if defined(HAVE_MALLOC_TRIM) && !HAVE_DECL_MALLOC_TRIM
int malloc_trim(size_t pad);
#endif
}

namespace SciTECO {

static gchar *filename_complete(const gchar *filename, gchar completed = ' ',
                                GFileTest file_test = G_FILE_TEST_EXISTS);
static gchar *symbol_complete(SymbolList &list, const gchar *symbol,
			      gchar completed = ' ');

static const gchar *last_occurrence(const gchar *str,
				    const gchar *chars = " \t\v\r\n\f<>,;@");
static inline gboolean filename_is_dir(const gchar *filename);

/** Current command line. */
Cmdline cmdline;

/** Last terminated command line */
static Cmdline last_cmdline;

/**
 * Specifies whether the immediate editing modifier
 * is enabled/disabled.
 * It can be toggled with the ^G immediate editing command
 * and influences the undo/redo direction and function of the
 * TAB key.
 */
static bool modifier_enabled = false;

bool quit_requested = false;

namespace States {
	StateSaveCmdline save_cmdline;
}

#if 0
Cmdline *
copy(void) const
{
	Cmdline *c = new Cmdline();

	if (str)
		c->str = g_memdup(str, len+rubout_len);
	c->len = len;
	c->rubout_len = rubout_len;

	return c;
}
#endif

/**
 * Throws a command line based on the command line
 * replacement register.
 * It is catched by Cmdline::keypress() to actually
 * perform the command line update.
 */
void
Cmdline::replace(void)
{
	QRegister *cmdline_reg = QRegisters::globals[CTL_KEY_ESC_STR];
	/* use heap object to avoid copy constructors etc. */
	Cmdline *new_cmdline = new Cmdline();

	/* FIXME: does not handle null bytes */
	new_cmdline->str = cmdline_reg->get_string();
	new_cmdline->len = strlen(new_cmdline->str);
	new_cmdline->rubout_len = 0;

	/*
	 * Search for first differing character in old and
	 * new command line. This avoids unnecessary rubouts
	 * and insertions when the command line is updated.
	 */
	for (new_cmdline->pc = 0;
	     new_cmdline->pc < len && new_cmdline->pc < new_cmdline->len &&
	     str[new_cmdline->pc] == new_cmdline->str[new_cmdline->pc];
	     new_cmdline->pc++);

	throw new_cmdline;
}

/**
 * Insert string into command line and execute
 * it immediately.
 * It already handles command line replacement and will
 * only throw SciTECO::Error.
 *
 * @param src String to insert (null-terminated).
 *            NULL inserts a character from the previously
 *            rubbed out command line.
 */
void
Cmdline::insert(const gchar *src)
{
	Cmdline old_cmdline;
	guint repl_pc = 0;

	macro_pc = pc = len;

	if (!src) {
		if (rubout_len) {
			len++;
			rubout_len--;
		}
	} else {
		size_t src_len = strlen(src);

		if (src_len <= rubout_len && !strncmp(str+len, src, src_len)) {
			len += src_len;
			rubout_len -= src_len;
		} else {
			if (rubout_len)
				/* automatically disable immediate editing modifier */
				modifier_enabled = false;

			String::append(str, len, src);
			len += src_len;
			rubout_len = 0;
		}
	}

	/*
	 * Parse/execute characters, one at a time so
	 * undo tokens get emitted for the corresponding characters.
	 */
	while (pc < len) {
		try {
			Execute::step(str, pc+1);
		} catch (Cmdline *new_cmdline) {
			/*
			 * Result of command line replacement (}):
			 * Exchange command lines, avoiding
			 * deep copying
			 */
			undo.pop(new_cmdline->pc);

			old_cmdline = *this;
			*this = *new_cmdline;
			new_cmdline->str = NULL;
			macro_pc = repl_pc = pc;

			delete new_cmdline;
			continue;
		} catch (Error &error) {
			error.add_frame(new Error::ToplevelFrame());
			error.display_short();

			if (old_cmdline.str) {
				/*
				 * Error during command-line replacement.
				 * Replay previous command-line.
				 * This avoids deep copying.
				 */
				undo.pop(repl_pc);

				g_free(str);
				*this = old_cmdline;
				old_cmdline.str = NULL;
				macro_pc = pc = repl_pc;

				/* rubout cmdline replacement command */
				len--;
				rubout_len++;
				continue;
			}

			/* error is handled in Cmdline::keypress() */
			throw;
		}

		pc++;
	}
}

void
Cmdline::keypress(gchar key)
{
	/*
	 * Cleanup messages,etc...
	 */
	interface.msg_clear();

	/*
	 * Process immediate editing commands, inserting
	 * characters as necessary into the command line.
	 */
	try {
		States::current->process_edit_cmd(key);
	} catch (Return) {
		/*
		 * Return from top-level macro, results
		 * in command line termination.
		 * The return "arguments" are currently
		 * ignored.
		 */
		g_assert(States::current == &States::start);

		interface.popup_clear();

		if (quit_requested)
			/* cought by user interface */
			throw Quit();

		undo.clear();
		/* also empties all Scintilla undo buffers */
		ring.set_scintilla_undo(true);
		QRegisters::view.set_scintilla_undo(true);
		Goto::table->clear();
		expressions.clear();
		loop_stack.clear();

		last_cmdline = *this;
		str = NULL;
		len = rubout_len = 0;

#ifdef HAVE_MALLOC_TRIM
		/*
		 * Glibc/Linux-only optimization: Undo stacks can grow very
		 * large - sometimes large enough to make the system
		 * swap and become unresponsive.
		 * This shrink the program break after lots of memory has
		 * been freed, reducing the virtual memory size and aiding
		 * in recovering from swapping issues.
		 */
		malloc_trim(0);
#endif
	} catch (Error &error) {
		/*
		 * NOTE: Error message already displayed in
		 * Cmdline::insert().
		 *
		 * Undo tokens may have been emitted
		 * (or had to be) before the exception
		 * is thrown. They must be executed so
		 * as if the character had never been
		 * inserted.
		 */
		undo.pop(pc);
		rubout_len += len-pc;
		len = pc;
		/* program counter could be messed up */
		macro_pc = len;
	}

	/*
	 * Echo command line
	 */
	interface.cmdline_update(this);
}

void
Cmdline::fnmacro(const gchar *name)
{
	gchar macro_name[1 + strlen(name) + 1];
	QRegister *reg;
	gchar *macro;

	if (!(Flags::ed & Flags::ED_FNKEYS))
		/* function key macros disabled */
		goto default_action;

	macro_name[0] = CTL_KEY('F');
	g_strlcpy(macro_name + 1, name, sizeof(macro_name) - 1);

	reg = QRegisters::globals[macro_name];
	if (!reg)
		/* macro undefined */
		goto default_action;

	if (reg->get_integer() & States::current->get_fnmacro_mask())
		return;

	macro = reg->get_string();
	try {
		keypress(macro);
	} catch (...) {
		/* could be "Quit" for instance */
		g_free(macro);
		throw;
	}
	g_free(macro);

	return;

	/*
	 * Most function key macros have no default action,
	 * except "CLOSE" which quits the application
	 * (this may loose unsaved data but is better than
	 * not doing anything if the user closes the window).
	 * NOTE: Doing the check here is less efficient than
	 * doing it in the UI implementations, but defines
	 * the default actions centrally.
	 * Also, fnmacros are only handled after key presses.
	 */
default_action:
	if (!strcmp(name, "CLOSE"))
		throw Quit();
}

static gchar *
filename_complete(const gchar *filename, gchar completed,
                  GFileTest file_test)
{
	gchar *filename_expanded;
	gsize filename_len;
	gchar *dirname, *basename, dir_sep;
	gsize dirname_len;
	const gchar *cur_basename;

	GDir *dir;
	GSList *files = NULL;
	guint files_len = 0;
	gchar *insert = NULL;
	gsize prefix_len = 0;

	if (Globber::is_pattern(filename))
		return NULL;

	filename_expanded = expand_path(filename);
	filename_len = strlen(filename_expanded);

	/*
	 * Derive base and directory names.
	 * We do not use g_path_get_basename() or g_path_get_dirname()
	 * since we need strict suffixes and prefixes of filename
	 * in order to construct paths of entries in dirname
	 * that are suitable for auto completion.
	 */
	dirname_len = file_get_dirname_len(filename_expanded);
	dirname = g_strndup(filename_expanded, dirname_len);
	basename = filename_expanded + dirname_len;

	dir = g_dir_open(dirname_len ? dirname : ".", 0, NULL);
	if (!dir) {
		g_free(dirname);
		g_free(filename_expanded);
		return NULL;
	}

	/*
	 * On Windows, both forward and backslash
	 * directory separators are allowed in directory
	 * names passed to glib.
	 * To imitate glib's behaviour, we use
	 * the last valid directory separator in `filename_expanded`
	 * to generate new separators.
	 * This also allows forward-slash auto-completion
	 * on Windows.
	 */
	dir_sep = dirname_len ? dirname[dirname_len-1]
	                      : G_DIR_SEPARATOR;

	while ((cur_basename = g_dir_read_name(dir))) {
		gchar *cur_filename;

		if (!g_str_has_prefix(cur_basename, basename))
			continue;

		/*
		 * dirname contains any directory separator,
		 * so g_strconcat() works here.
		 */
		cur_filename = g_strconcat(dirname, cur_basename, NIL);

		/*
		 * NOTE: This avoids g_file_test() for G_FILE_TEST_EXISTS
		 * since the file we process here should always exist.
		 */
		if ((!*basename && !file_is_visible(cur_filename)) ||
		    (file_test != G_FILE_TEST_EXISTS &&
		     !g_file_test(cur_filename, file_test))) {
			g_free(cur_filename);
			continue;
		}

		if (file_test == G_FILE_TEST_IS_DIR ||
		    g_file_test(cur_filename, G_FILE_TEST_IS_DIR))
			String::append(cur_filename, dir_sep);

		files = g_slist_prepend(files, cur_filename);

		if (g_slist_next(files)) {
			const gchar *other_file = (gchar *)g_slist_next(files)->data;
			gsize len = String::diff(other_file + filename_len,
						 cur_filename + filename_len);
			if (len < prefix_len)
				prefix_len = len;
		} else {
			prefix_len = strlen(cur_filename + filename_len);
		}

		files_len++;
	}
	if (prefix_len > 0)
		insert = g_strndup((gchar *)files->data + filename_len, prefix_len);

	g_dir_close(dir);
	g_free(dirname);
	g_free(filename_expanded);

	if (!insert && files_len > 1) {
		files = g_slist_sort(files, (GCompareFunc)g_strcmp0);

		for (GSList *file = files; file; file = g_slist_next(file)) {
			InterfaceCurrent::PopupEntryType type;
			bool is_buffer = false;

			if (filename_is_dir((gchar *)file->data)) {
				type = InterfaceCurrent::POPUP_DIRECTORY;
			} else {
				type = InterfaceCurrent::POPUP_FILE;
				/* FIXME: inefficient */
				is_buffer = ring.find((gchar *)file->data);
			}

			interface.popup_add(type, (gchar *)file->data,
					    is_buffer);
		}

		interface.popup_show();
	} else if (completed && files_len == 1 &&
	           !filename_is_dir((gchar *)files->data)) {
		/*
		 * FIXME: If we are completing only directories,
		 * we can theoretically insert the completed character
		 * after directories without subdirectories
		 */
		String::append(insert, completed);
	}

	g_slist_free_full(files, g_free);

	return insert;
}

static gchar *
symbol_complete(SymbolList &list, const gchar *symbol, gchar completed)
{
	GList *glist;
	guint glist_len = 0;
	gchar *insert = NULL;
	gsize symbol_len;
	gsize prefix_len = 0;

	if (!symbol)
		symbol = "";
	symbol_len = strlen(symbol);

	glist = list.get_glist();
	if (!glist)
		return NULL;
	glist = g_list_copy(glist);
	if (!glist)
		return NULL;
	/* NOTE: element data must not be freed */

	for (GList *entry = g_list_first(glist), *next = g_list_next(entry);
	     entry != NULL;
	     entry = next, next = entry ? g_list_next(entry) : NULL) {
		if (!g_str_has_prefix((gchar *)entry->data, symbol)) {
			glist = g_list_delete_link(glist, entry);
			continue;
		}

		gsize len = String::diff((gchar *)glist->data + symbol_len,
					 (gchar *)entry->data + symbol_len);
		if (!prefix_len || len < prefix_len)
			prefix_len = len;

		glist_len++;
	}
	if (prefix_len > 0)
		insert = g_strndup((gchar *)glist->data + symbol_len, prefix_len);

	if (!insert && glist_len > 1) {
		for (GList *entry = g_list_first(glist);
		     entry != NULL;
		     entry = g_list_next(entry)) {
			interface.popup_add(InterfaceCurrent::POPUP_PLAIN,
					    (gchar *)entry->data);
		}

		interface.popup_show();
	} else if (glist_len == 1) {
		String::append(insert, completed);
	}

	g_list_free(glist);

	return insert;
}

/*
 * Commandline key processing.
 *
 * These are all the implementations of State::process_edit_cmd().
 * It makes sense to use virtual methods for key processing, as it is
 * largely state-dependant; but it defines interactive-mode-only
 * behaviour which can be kept isolated from the rest of the states'
 * implementation.
 */

void
State::process_edit_cmd(gchar key)
{
	switch (key) {
	case '\n': /* insert EOL sequence */
		interface.popup_clear();

		if (Flags::ed & Flags::ED_AUTOEOL)
			cmdline.insert("\n");
		else
			cmdline.insert(get_eol_seq(interface.ssm(SCI_GETEOLMODE)));
		return;

	case CTL_KEY('G'): /* toggle immediate editing modifier */
		interface.popup_clear();

		modifier_enabled = !modifier_enabled;
		interface.msg(InterfaceCurrent::MSG_INFO,
			      "Immediate editing modifier is now %s.",
			      modifier_enabled ? "enabled" : "disabled");
		return;

	case CTL_KEY('H'): /* rubout/reinsert character */
		interface.popup_clear();

		if (modifier_enabled)
			/* re-insert character */
			cmdline.insert();
		else
			/* rubout character */
			cmdline.rubout();
		return;

	case CTL_KEY('W'): /* rubout/reinsert command */
		interface.popup_clear();

		if (modifier_enabled) {
			/* reinsert command */
			do
				cmdline.insert();
			while (!States::is_start() && cmdline.rubout_len);
		} else {
			/* rubout command */
			do
				cmdline.rubout();
			while (!States::is_start());
		}
		return;

#ifdef SIGTSTP
	case CTL_KEY('Z'):
		/*
		 * <CTL/Z> does not raise signal if handling of
		 * special characters temporarily disabled in terminal
		 * (Curses), or command-line is detached from
		 * terminal (GTK+).
		 * This does NOT change the state of the popup window.
		 */
		raise(SIGTSTP);
		return;
#endif
	}

	interface.popup_clear();
	cmdline.insert(key);
}

void
StateCaseInsensitive::process_edit_cmd(gchar key)
{
	if (Flags::ed & Flags::ED_AUTOCASEFOLD)
		/* will not modify non-letter keys */
		key = g_ascii_islower(key) ? g_ascii_toupper(key)
		                           : g_ascii_tolower(key);

	State::process_edit_cmd(key);
}

void
StateExpectString::process_edit_cmd(gchar key)
{
	switch (key) {
	case CTL_KEY('W'): { /* rubout/reinsert word */
		interface.popup_clear();

		gchar wchars[interface.ssm(SCI_GETWORDCHARS)];
		interface.ssm(SCI_GETWORDCHARS, 0, (sptr_t)wchars);

		if (modifier_enabled) {
			/* reinsert word chars */
			while (States::current == this && cmdline.rubout_len &&
			       strchr(wchars, cmdline.str[cmdline.len]))
				cmdline.insert();

			/* reinsert non-word chars */
			while (States::current == this && cmdline.rubout_len &&
			       !strchr(wchars, cmdline.str[cmdline.len]))
				cmdline.insert();
			return;
		}

		if (strings[0] && *strings[0]) {
			/* rubout non-word chars */
			while (strings[0] && *strings[0] &&
			       !strchr(wchars, cmdline.str[cmdline.len-1]))
				cmdline.rubout();

			/* rubout word chars */
			while (strings[0] && *strings[0] &&
			       strchr(wchars, cmdline.str[cmdline.len-1]))
				cmdline.rubout();
			return;
		}

		/*
		 * Otherwise, the entire command string will
		 * be rubbed out.
		 */
		break;
	}

	case CTL_KEY('U'): /* rubout/reinsert string */
		interface.popup_clear();

		if (modifier_enabled) {
			/* reinsert string */
			while (States::current == this && cmdline.rubout_len)
				cmdline.insert();
		} else {
			/* rubout string */
			while (strings[0] && *strings[0])
				cmdline.rubout();
		}
		return;

	case '\t': /* autocomplete file name */
		if (modifier_enabled) {
			/*
			 * TODO: In insertion commands, we can autocomplete
			 * the string at the buffer cursor.
			 */
			/* autocomplete filename using string argument */
			if (interface.popup_is_shown()) {
				/* cycle through popup pages */
				interface.popup_show();
				return;
			}

			const gchar *filename = last_occurrence(strings[0]);
			gchar *new_chars = filename_complete(filename);

			if (new_chars)
				cmdline.insert(new_chars);
			g_free(new_chars);

			/* may be reset if there was a rubbed out command line */
			modifier_enabled = true;
			return;
		}

		if (machine.qregspec_machine) {
			if (interface.popup_is_shown()) {
				/* cycle through popup pages */
				interface.popup_show();
				return;
			}

			gchar *new_chars = machine.qregspec_machine->auto_complete();

			if (new_chars)
				cmdline.insert(new_chars);
			g_free(new_chars);
			return;
		}
		break;
	}

	State::process_edit_cmd(key);
}

void
StateInsert::process_edit_cmd(gchar key)
{
	gint spaces;

	switch (key) {
	case '\t': /* insert <TAB> indention */
		if (modifier_enabled || interface.ssm(SCI_GETUSETABS))
			break;

		interface.popup_clear();

		/* insert soft tabs */
		spaces = interface.ssm(SCI_GETTABWIDTH);
		spaces -= interface.ssm(SCI_GETCOLUMN,
		                        interface.ssm(SCI_GETCURRENTPOS)) % spaces;

		while (spaces--)
			cmdline.insert(' ');
		return;
	}

	StateExpectString::process_edit_cmd(key);
}

void
StateExpectFile::process_edit_cmd(gchar key)
{
	gchar *new_chars;

	switch (key) {
	case CTL_KEY('W'): /* rubout/reinsert file names including directories */
		interface.popup_clear();

		if (modifier_enabled) {
			/* reinsert one level of file name */
			while (States::current == this && cmdline.rubout_len &&
			       !G_IS_DIR_SEPARATOR(cmdline.str[cmdline.len]))
				cmdline.insert();

			/* reinsert final directory separator */
			if (States::current == this && cmdline.rubout_len &&
			    G_IS_DIR_SEPARATOR(cmdline.str[cmdline.len]))
				cmdline.insert();
			return;
		}

		if (strings[0] && *strings[0]) {
			/* rubout directory separator */
			if (strings[0] && *strings[0] &&
			    G_IS_DIR_SEPARATOR(cmdline.str[cmdline.len-1]))
				cmdline.rubout();

			/* rubout one level of file name */
			while (strings[0] && *strings[0] &&
			       !G_IS_DIR_SEPARATOR(cmdline.str[cmdline.len-1]))
				cmdline.rubout();
			return;
		}

		/*
		 * Rub out entire command instead of
		 * rubbing out nothing.
		 */
		break;

	case '\t': /* autocomplete file name */
		if (modifier_enabled)
			break;

		if (interface.popup_is_shown()) {
			/* cycle through popup pages */
			interface.popup_show();
			return;
		}

		new_chars = filename_complete(strings[0],
		                              escape_char == '{' ? '\0' : escape_char);
		if (new_chars)
			cmdline.insert(new_chars);
		g_free(new_chars);
		return;
	}

	StateExpectString::process_edit_cmd(key);
}

void
StateExpectDir::process_edit_cmd(gchar key)
{
	gchar *new_chars;

	switch (key) {
	case '\t': /* autocomplete directory */
		if (modifier_enabled)
			break;

		if (interface.popup_is_shown()) {
			/* cycle through popup pages */
			interface.popup_show();
			return;
		}

		new_chars = filename_complete(strings[0], '\0',
		                              G_FILE_TEST_IS_DIR);
		if (new_chars)
			cmdline.insert(new_chars);
		g_free(new_chars);
		return;
	}

	StateExpectFile::process_edit_cmd(key);
}

void
StateExpectQReg::process_edit_cmd(gchar key)
{
	gchar *new_chars;

	switch (key) {
	case '\t': /* autocomplete Q-Register name */
		if (modifier_enabled)
			break;

		if (interface.popup_is_shown()) {
			/* cycle through popup pages */
			interface.popup_show();
			return;
		}

		new_chars = machine.auto_complete();
		if (new_chars)
			cmdline.insert(new_chars);
		g_free(new_chars);
		return;
	}

	State::process_edit_cmd(key);
}

void
StateExecuteCommand::process_edit_cmd(gchar key)
{
	gchar *new_chars;

	switch (key) {
	case '\t': /* autocomplete symbol or file name */
		if (modifier_enabled)
			break;

		/*
		 * In the EC command, <TAB> completes files just like ^T
		 * TODO: Implement shell-command completion by iterating
		 * executables in $PATH
		 */
		if (interface.popup_is_shown()) {
			/* cycle through popup pages */
			interface.popup_show();
			return;
		}

		new_chars = filename_complete(last_occurrence(strings[0]));
		if (new_chars)
			cmdline.insert(new_chars);
		g_free(new_chars);
		return;
	}

	StateExpectString::process_edit_cmd(key);
}

void
StateScintilla_symbols::process_edit_cmd(gchar key)
{
	switch (key) {
	case '\t': { /* autocomplete Scintilla symbol */
		if (modifier_enabled)
			break;

		if (interface.popup_is_shown()) {
			/* cycle through popup pages */
			interface.popup_show();
			return;
		}

		const gchar *symbol = last_occurrence(strings[0], ",");
		SymbolList &list = symbol == strings[0]
					? Symbols::scintilla
					: Symbols::scilexer;
		gchar *new_chars = symbol_complete(list, symbol, ',');

		if (new_chars)
			cmdline.insert(new_chars);
		g_free(new_chars);
		return;
	}
	}

	StateExpectString::process_edit_cmd(key);
}

void
StateGotoCmd::process_edit_cmd(gchar key)
{
	switch (key) {
	case '\t': { /* autocomplete goto label */
		if (modifier_enabled)
			break;

		if (interface.popup_is_shown()) {
			/* cycle through popup pages */
			interface.popup_show();
			return;
		}

		const gchar *label = last_occurrence(strings[0], ",");
		gchar *new_chars = Goto::table->auto_complete(label);

		if (new_chars)
			cmdline.insert(new_chars);
		g_free(new_chars);
		return;
	}
	}

	StateExpectString::process_edit_cmd(key);
}

void
StateGetHelp::process_edit_cmd(gchar key)
{
	switch (key) {
	case '\t': { /* autocomplete help term */
		if (modifier_enabled)
			break;

		if (interface.popup_is_shown()) {
			/* cycle through popup pages */
			interface.popup_show();
			return;
		}

		gchar complete = escape_char == '{' ? '\0' : escape_char;
		gchar *new_chars = help_index.auto_complete(strings[0], complete);

		if (new_chars)
			cmdline.insert(new_chars);
		g_free(new_chars);
		return;
	}
	}

	StateExpectString::process_edit_cmd(key);
}

/*
 * Command states
 */

/*$ *q
 * *q -- Save last command line
 *
 * Only at the very beginning of a command-line, this command
 * may be used to save the last command line as a string in
 * Q-Register <q>.
 */
State *
StateSaveCmdline::got_register(QRegister *reg)
{
	machine.reset();

	BEGIN_EXEC(&States::start);
	reg->undo_set_string();
	reg->set_string(last_cmdline.str, last_cmdline.len);
	return &States::start;
}

/*
 * Auxiliary functions
 */

static const gchar *
last_occurrence(const gchar *str, const gchar *chars)
{
	if (!str)
		return NULL;

	while (*chars) {
		const gchar *p = strrchr(str, *chars++);
		if (p)
			str = p+1;
	}

	return str;
}

static inline gboolean
filename_is_dir(const gchar *filename)
{
	gchar c;

	if (!*filename)
		return false;

	c = filename[strlen(filename)-1];
	return G_IS_DIR_SEPARATOR(c);
}

} /* namespace SciTECO */
