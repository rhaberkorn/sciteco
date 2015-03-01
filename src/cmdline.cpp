/*
 * Copyright (C) 2012-2015 Robin Haberkorn
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
#include "undo.h"
#include "symbols.h"
#include "spawn.h"
#include "glob.h"
#include "error.h"
#include "cmdline.h"

namespace SciTECO {

static gchar *filename_complete(const gchar *filename, gchar completed = ' ');
static gchar *symbol_complete(SymbolList &list, const gchar *symbol,
			      gchar completed = ' ');

static const gchar *last_occurrence(const gchar *str,
				    const gchar *chars = " \t\v\r\n\f<>,;@");
static inline gboolean filename_is_dir(const gchar *filename);
static inline gchar derive_dir_separator(const gchar *filename);

/** Current command line. */
Cmdline cmdline;

/** Last terminated command line */
static Cmdline last_cmdline;

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
	QRegister *cmdline_reg = QRegisters::globals["\x1B"];
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

void
Cmdline::keypress(gchar key)
{
	Cmdline old_cmdline;

	gsize old_len = len;
	guint repl_pc = 0;

	/*
	 * Cleanup messages,etc...
	 */
	interface.msg_clear();

	/*
	 * Process immediate editing commands, inserting
	 * characters as necessary into the command line.
	 */
	if (process_edit_cmd(key))
		interface.popup_clear();

	/*
	 * Parse/execute characters, one at a time so
	 * undo tokens get emitted for the corresponding characters.
	 */
	macro_pc = pc = MIN(old_len, len);

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

			/*
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
			break;
		}

		pc++;
	}

	/*
	 * Echo command line
	 */
	interface.cmdline_update(this);
}

void
Cmdline::insert(const gchar *src)
{
	size_t src_len = strlen(src);

	if (src_len <= rubout_len && !strncmp(str+len, src, src_len)) {
		len += src_len;
		rubout_len -= src_len;
	} else {
		String::append(str, len, src);
		len += src_len;
		rubout_len = 0;
	}
}

bool
Cmdline::process_edit_cmd(gchar key)
{
	switch (key) {
	case '\b': /* rubout character */
		if (len)
			rubout();
		break;

	case CTL_KEY('W'): /* rubout word */
		if (States::is_string()) {
			gchar wchars[interface.ssm(SCI_GETWORDCHARS)];
			interface.ssm(SCI_GETWORDCHARS, 0, (sptr_t)wchars);

			/* rubout non-word chars */
			while (strings[0] && strlen(strings[0]) > 0 &&
			       !strchr(wchars, str[len-1]))
				rubout();

			/* rubout word chars */
			while (strings[0] && strlen(strings[0]) > 0 &&
			       strchr(wchars, str[len-1]))
				rubout();
		} else if (len) {
			do
				rubout();
			while (States::current != &States::start);
		}
		break;

	case CTL_KEY('U'): /* rubout string */
		if (States::is_string()) {
			while (strings[0] && strlen(strings[0]) > 0)
				rubout();
		} else {
			insert(key);
		}
		break;

	case CTL_KEY('T'): /* autocomplete file name */
		/*
		 * TODO: In insertion commands, we can autocomplete
		 * the string at the buffer cursor.
		 */
		if (States::is_string()) {
			if (interface.popup_is_shown()) {
				/* cycle through popup pages */
				interface.popup_show();
				return false;
			}

			const gchar *filename = last_occurrence(strings[0]);
			gchar *new_chars = filename_complete(filename);

			insert(new_chars);
			g_free(new_chars);

			if (interface.popup_is_shown())
				return false;
		} else {
			insert(key);
		}
		break;

	case '\t': /* autocomplete symbol or file name */
		if (States::is_insertion() && !interface.ssm(SCI_GETUSETABS)) {
			gint spaces = interface.ssm(SCI_GETTABWIDTH);

			spaces -= interface.ssm(SCI_GETCOLUMN,
			                        interface.ssm(SCI_GETCURRENTPOS)) % spaces;

			while (spaces--)
				insert(' ');
		} else if (States::is_file()) {
			if (interface.popup_is_shown()) {
				/* cycle through popup pages */
				interface.popup_show();
				return false;
			}

			gchar complete = escape_char == '{' ? ' ' : escape_char;
			gchar *new_chars = filename_complete(strings[0], complete);

			insert(new_chars);
			g_free(new_chars);

			if (interface.popup_is_shown())
				return false;
		} else if (States::current == &States::executecommand) {
			/*
			 * In the EC command, <TAB> completes files just like ^T
			 * TODO: Implement shell-command completion by iterating
			 * executables in $PATH
			 */
			if (interface.popup_is_shown()) {
				/* cycle through popup pages */
				interface.popup_show();
				return false;
			}

			const gchar *filename = last_occurrence(strings[0]);
			gchar *new_chars = filename_complete(filename);

			insert(new_chars);
			g_free(new_chars);

			if (interface.popup_is_shown())
				return false;
		} else if (States::current == &States::scintilla_symbols) {
			if (interface.popup_is_shown()) {
				/* cycle through popup pages */
				interface.popup_show();
				return false;
			}

			const gchar *symbol = last_occurrence(strings[0], ",");
			SymbolList &list = symbol == strings[0]
						? Symbols::scintilla
						: Symbols::scilexer;
			gchar *new_chars = symbol_complete(list, symbol, ',');

			insert(new_chars);
			g_free(new_chars);

			if (interface.popup_is_shown())
				return false;
		} else {
			insert(key);
		}
		break;

	case '\x1B': /* terminate command line */
		if (States::current == &States::start &&
		    str && str[len-1] == '\x1B') {
			if (Goto::skip_label) {
				interface.msg(InterfaceCurrent::MSG_ERROR,
					      "Label \"%s\" not found",
					      Goto::skip_label);
				break;
			}

			if (quit_requested)
				/* cought by user interface */
				throw Quit();

			undo.clear();
			/* also empties all Scintilla undo buffers */
			ring.set_scintilla_undo(true);
			QRegisters::view.set_scintilla_undo(true);
			Goto::table->clear();
			expressions.clear();

			last_cmdline = *this;
			str = NULL;
			len = rubout_len = 0;

			/*
			 * FIXME: Perhaps to the malloc_trim() here
			 * instead of in UndoStack::clear()
			 */
		} else {
			insert(key);
		}
		break;

#ifdef SIGTSTP
	case CTL_KEY('Z'):
		/*
		 * <CTL/Z> does not raise signal if handling of
		 * special characters temporarily disabled in terminal
		 * (Curses), or command-line is detached from
		 * terminal (GTK+)
		 */
		raise(SIGTSTP);
		break;
#endif

	default:
		insert(key);
	}

	return true;
}

void
Cmdline::fnmacro(const gchar *name)
{
	/* FIXME: check again if function keys are enabled */
	gchar macro_name[1 + strlen(name) + 1];
	QRegister *reg;

	macro_name[0] = CTL_KEY('F');
	g_strlcpy(macro_name + 1, name, sizeof(macro_name) - 1);

	reg = QRegisters::globals[macro_name];
	if (reg) {
		gchar *macro = reg->get_string();
		keypress(macro);
		g_free(macro);
	}
}

const gchar *
get_eol(void)
{
	switch (interface.ssm(SCI_GETEOLMODE)) {
	case SC_EOL_CR:
		return "\r";
	case SC_EOL_CRLF:
		return "\r\n";
	case SC_EOL_LF:
	default:
		return "\n";
	}
}

static gchar *
filename_complete(const gchar *filename, gchar completed)
{
	gchar *dirname;
	const gchar *basename, *cur_basename;
	GDir *dir;
	GSList *files = NULL;
	guint files_len = 0;
	gchar *insert = NULL;
	gsize filename_len;
	gsize prefix_len = 0;

	gchar dir_sep[2];

	if (!filename)
		filename = "";
	filename_len = strlen(filename);

	if (is_glob_pattern(filename))
		return NULL;

	/*
	 * On Windows, both forward and backslash
	 * directory separators are allowed in directory
	 * names passed to glib.
	 * To imitate glib's behaviour, we use
	 * the last valid directory separator in `filename`
	 * to generate new separators.
	 * This also allows forward-slash auto-completion
	 * on Windows.
	 */
	dir_sep[0] = derive_dir_separator(filename);
	dir_sep[1] = '\0';

	dirname = g_path_get_dirname(filename);
	dir = g_dir_open(dirname, 0, NULL);
	if (!dir) {
		g_free(dirname);
		return NULL;
	}
	if (*dirname != *filename)
		*dirname = '\0';

	basename = strrchr(filename, *dir_sep);
	if (basename)
		basename++;
	else
		basename = filename;

	while ((cur_basename = g_dir_read_name(dir))) {
		gchar *cur_filename = g_build_path(dir_sep, dirname, cur_basename, NIL);

		if (!g_str_has_prefix(cur_basename, basename) ||
		    (!*basename && !file_is_visible(cur_filename))) {
			g_free(cur_filename);
			continue;
		}

		if (g_file_test(cur_filename, G_FILE_TEST_IS_DIR))
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

	g_free(dirname);
	g_dir_close(dir);

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
	} else if (files_len == 1 && !filename_is_dir((gchar *)files->data)) {
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
 * Command states
 */

/*$
 * *q -- Save last command line
 *
 * Only at the very beginning of a command-line, this command
 * may be used to save the last command line as a string in
 * Q-Register <q>.
 */
State *
StateSaveCmdline::got_register(QRegister &reg)
{
	BEGIN_EXEC(&States::start);

	reg.undo_set_string();
	reg.set_string(last_cmdline.str, last_cmdline.len);

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

#ifdef G_OS_WIN32

static inline gchar
derive_dir_separator(const gchar *filename)
{
	gchar sep = G_DIR_SEPARATOR;

	while (*filename) {
		if (G_IS_DIR_SEPARATOR(*filename))
			sep = *filename;
		filename++;
	}

	return sep;
}

#else /* !G_OS_WIN32 */

static inline gchar
derive_dir_separator(const gchar *filename)
{
	return G_DIR_SEPARATOR;
}

#endif

} /* namespace SciTECO */
