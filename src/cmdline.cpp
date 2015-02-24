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

static inline const gchar *process_edit_cmd(gchar key);
static gchar *macro_echo(const gchar *macro);

static gchar *filename_complete(const gchar *filename, gchar completed = ' ');
static gchar *symbol_complete(SymbolList &list, const gchar *symbol,
			      gchar completed = ' ');

static const gchar *last_occurrence(const gchar *str,
				    const gchar *chars = " \t\v\r\n\f<>,;@");
static inline gboolean filename_is_dir(const gchar *filename);
static inline gchar derive_dir_separator(const gchar *filename);

gchar *cmdline = NULL;
gint cmdline_pos = 0;
static gchar *last_cmdline = NULL;

bool quit_requested = false;

namespace States {
	StateSaveCmdline save_cmdline;
}

void
cmdline_keypress(gchar key)
{
	gchar *old_cmdline = NULL;
	gint repl_pos = 0;

	const gchar *insert;
	gchar *echo;

	/*
	 * Cleanup messages,etc...
	 */
	interface.msg_clear();

	/*
	 * Process immediate editing commands.
	 * It may clear/hide the popup.
	 */
	insert = process_edit_cmd(key);

	/*
	 * Parse/execute characters, one at a time so
	 * undo tokens get emitted for the corresponding characters.
	 */
	cmdline_pos = cmdline ? strlen(cmdline)+1 : 1;
	String::append(cmdline, insert);

	while (cmdline[cmdline_pos-1]) {
		try {
			Execute::step(cmdline, cmdline_pos);
		} catch (ReplaceCmdline &r) {
			undo.pop(r.pos);

			old_cmdline = cmdline;
			cmdline = r.new_cmdline;
			cmdline_pos = repl_pos = r.pos;
			macro_pc = r.pos-1;
			continue;
		} catch (Error &error) {
			error.add_frame(new Error::ToplevelFrame());
			error.display_short();

			if (old_cmdline) {
				undo.pop(repl_pos);

				g_free(cmdline);
				cmdline = old_cmdline;
				cmdline[strlen(cmdline)-1] = '\0';
				old_cmdline = NULL;
				cmdline_pos = repl_pos;
				macro_pc = repl_pos-1;
				continue;
			}
			/*
			 * Undo tokens may have been emitted
			 * (or had to be) before the exception
			 * is thrown. They must be executed so
			 * as if the character had never been
			 * inserted.
			 */
			undo.pop(cmdline_pos);
			cmdline[cmdline_pos-1] = '\0';
			/* program counter could be messed up */
			macro_pc = cmdline_pos - 1;
			break;
		}

		cmdline_pos++;
	}

	g_free(old_cmdline);

	/*
	 * Echo command line
	 */
	echo = macro_echo(cmdline);
	interface.cmdline_update(echo);
	g_free(echo);
}

static inline const gchar *
process_edit_cmd(gchar key)
{
	static gchar insert[255];
	gint cmdline_len = cmdline ? strlen(cmdline) : 0;
	bool clear_popup = true;

	insert[0] = key;
	insert[1] = '\0';

	switch (key) {
	case '\b':
		if (cmdline_len) {
			undo.pop(cmdline_len);
			cmdline[cmdline_len - 1] = '\0';
			macro_pc--;
		}
		*insert = '\0';
		break;

	case CTL_KEY('W'):
		if (States::is_string()) {
			gchar wchars[interface.ssm(SCI_GETWORDCHARS)];
			interface.ssm(SCI_GETWORDCHARS, 0, (sptr_t)wchars);

			/* rubout non-word chars */
			while (strings[0] && strlen(strings[0]) > 0 &&
			       !strchr(wchars, cmdline[macro_pc-1]))
				undo.pop(macro_pc--);

			/* rubout word chars */
			while (strings[0] && strlen(strings[0]) > 0 &&
			       strchr(wchars, cmdline[macro_pc-1]))
				undo.pop(macro_pc--);
		} else if (cmdline_len) {
			do
				undo.pop(macro_pc--);
			while (States::current != &States::start);
		}
		cmdline[macro_pc] = '\0';
		*insert = '\0';
		break;

	case CTL_KEY('U'):
		if (States::is_string()) {
			while (strings[0] && strlen(strings[0]) > 0)
				undo.pop(macro_pc--);
			cmdline[macro_pc] = '\0';
			*insert = '\0';
		}
		break;

	case CTL_KEY('T'):
		if (States::is_string()) {
			*insert = '\0';
			if (interface.popup_is_shown()) {
				/* cycle through popup pages */
				interface.popup_show();
				clear_popup = false;
				break;
			}

			const gchar *filename = last_occurrence(strings[0]);
			gchar *new_chars = filename_complete(filename);

			clear_popup = !interface.popup_is_shown();

			if (new_chars)
				g_stpcpy(insert, new_chars);
			g_free(new_chars);
		}
		break;

	case '\t':
		if (States::is_insertion()) {
			if (!interface.ssm(SCI_GETUSETABS)) {
				gint len = interface.ssm(SCI_GETTABWIDTH);

				len -= interface.ssm(SCI_GETCOLUMN,
				                     interface.ssm(SCI_GETCURRENTPOS)) % len;

				memset(insert, ' ', len);
				insert[len] = '\0';
			}
		} else if (States::is_file()) {
			*insert = '\0';
			if (interface.popup_is_shown()) {
				/* cycle through popup pages */
				interface.popup_show();
				clear_popup = false;
				break;
			}

			gchar complete = escape_char == '{' ? ' ' : escape_char;
			gchar *new_chars = filename_complete(strings[0], complete);

			clear_popup = !interface.popup_is_shown();

			if (new_chars)
				g_stpcpy(insert, new_chars);
			g_free(new_chars);
		} else if (States::current == &States::executecommand) {
			/*
			 * In the EC command, <TAB> completes files just like ^T
			 * TODO: Implement shell-command completion by iterating
			 * executables in $PATH
			 */
			*insert = '\0';
			if (interface.popup_is_shown()) {
				/* cycle through popup pages */
				interface.popup_show();
				clear_popup = false;
				break;
			}

			const gchar *filename = last_occurrence(strings[0]);
			gchar *new_chars = filename_complete(filename);

			clear_popup = !interface.popup_is_shown();

			if (new_chars)
				g_stpcpy(insert, new_chars);
			g_free(new_chars);
		} else if (States::current == &States::scintilla_symbols) {
			*insert = '\0';
			if (interface.popup_is_shown()) {
				/* cycle through popup pages */
				interface.popup_show();
				clear_popup = false;
				break;
			}

			const gchar *symbol = last_occurrence(strings[0], ",");
			SymbolList &list = symbol == strings[0]
						? Symbols::scintilla
						: Symbols::scilexer;
			gchar *new_chars = symbol_complete(list, symbol, ',');

			clear_popup = !interface.popup_is_shown();

			if (new_chars)
				g_stpcpy(insert, new_chars);
			g_free(new_chars);
		}

		break;

	case '\x1B':
		if (States::current == &States::start &&
		    cmdline && cmdline[cmdline_len - 1] == '\x1B') {
			*insert = '\0';

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

			g_free(last_cmdline);
			last_cmdline = cmdline;
			cmdline = NULL;
			macro_pc = 0;
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
		*insert = '\0';
		break;
#endif
	}

	if (clear_popup)
		interface.popup_clear();

	return insert;
}

void
cmdline_fnmacro(const gchar *name)
{
	gchar macro_name[1 + strlen(name) + 1];
	QRegister *reg;

	macro_name[0] = CTL_KEY('F');
	g_strlcpy(macro_name + 1, name, sizeof(macro_name) - 1);

	reg = QRegisters::globals[macro_name];
	if (reg) {
		gchar *macro = reg->get_string();
		cmdline_keypress(macro);
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
macro_echo(const gchar *macro)
{
	gchar *result, *rp;

	if (!macro)
		return g_strdup("");

	rp = result = (gchar *)g_malloc(strlen(macro)*5 + 1);

	for (const gchar *p = macro; *p; p++) {
		switch (*p) {
		case '\x1B':
			*rp++ = '$';
			break;
		case '\r':
			rp = g_stpcpy(rp, "<CR>");
			break;
		case '\n':
			rp = g_stpcpy(rp, "<LF>");
			break;
		case '\t':
			rp = g_stpcpy(rp, "<TAB>");
			break;
		default:
			if (IS_CTL(*p)) {
				*rp++ = '^';
				*rp++ = CTL_ECHO(*p);
			} else {
				*rp++ = *p;
			}
		}
	}
	*rp = '\0';

	return result;
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
	reg.set_string(last_cmdline);

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
