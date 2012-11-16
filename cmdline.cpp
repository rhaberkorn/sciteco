#include <string.h>
#include <stdlib.h>

#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>

#include "sciteco.h"
#include "interface.h"
#include "parser.h"
#include "qbuffers.h"
#include "goto.h"
#include "undo.h"

static inline const gchar *process_edit_cmd(gchar key);
static gchar *macro_echo(const gchar *macro);
static gchar *filename_complete(const gchar *filename, gchar completed = ' ');

static const gchar *last_occurrence(const gchar *str,
				    const gchar *chars = " \t\v\r\n\f<>,;@");
static inline gboolean filename_is_dir(const gchar *filename);

gchar *cmdline = NULL;

bool quit_requested = false;

void
cmdline_keypress(gchar key)
{
	const gchar *insert;
	gint old_cmdline_len = 0;
	gchar *echo;

	/*
	 * Cleanup messages, popups, etc...
	 */
	interface.popup_clear();

	/*
	 * Process immediate editing commands
	 */
	insert = process_edit_cmd(key);

	/*
	 * Parse/execute characters
	 */
	if (cmdline) {
		old_cmdline_len = strlen(cmdline);
		cmdline = (gchar *)g_realloc(cmdline, old_cmdline_len +
						      strlen(insert) + 1);
	} else {
		cmdline = (gchar *)g_malloc(strlen(insert) + 1);
		*cmdline = '\0';
	}

	for (const gchar *p = insert; *p; p++) {
		strcat(cmdline, (gchar[]){*p, '\0'});

		try {
			macro_execute(cmdline);
		} catch (...) {
			cmdline[old_cmdline_len] = '\0';
			break;
		}
	}

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

	insert[0] = '\0';

	switch (key) {
	case '\b':
		if (cmdline_len) {
			undo.pop(cmdline_len);
			cmdline[cmdline_len - 1] = '\0';
			macro_pc--;
		}
		break;

	case CTL_KEY('T'): {
		const gchar *filename = cmdline ? last_occurrence(cmdline) + 1
						: NULL;
		gchar *new_chars = filename_complete(filename);
		if (new_chars)
			g_stpcpy(insert, new_chars);
		g_free(new_chars);
		break;
	}

	case '\t':
		if (States::current == &States::editfile ||
		    States::current == &States::savefile ||
		    States::current == &States::loadqreg) {
			gchar *new_chars = filename_complete(strings[0], escape_char);
			if (new_chars)
				g_stpcpy(insert, new_chars);
			g_free(new_chars);
		} else {
			insert[0] = key;
			insert[1] = '\0';
		}
		break;

	case '\x1B':
		if (cmdline && cmdline[cmdline_len - 1] == '\x1B') {
			if (quit_requested) {
				/* FIXME */
				exit(EXIT_SUCCESS);
			}

			interface.ssm(SCI_EMPTYUNDOBUFFER);
			undo.clear();
			goto_table_clear();

			*cmdline = '\0';
			macro_pc = 0;
			break;
		}
		/* fall through */
	default:
		insert[0] = key;
		insert[1] = '\0';
	}

	return insert;
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
	gchar *dirname, *basename;
	GDir *dir;
	GList *files = NULL, *matching;
	GCompletion *completion;
	gchar *new_prefix;
	gchar *insert = NULL;

	if (!filename)
		filename = "";

	if (is_glob_pattern(filename))
		return NULL;

	dirname = g_path_get_dirname(filename);
	dir = g_dir_open(dirname, 0, NULL);
	if (!dir) {
		g_free(dirname);
		return NULL;
	}
	if (*dirname != *filename)
		*dirname = '\0';

	while ((basename = (gchar *)g_dir_read_name(dir))) {
		gchar *filename = g_build_filename(dirname, basename, NULL);

		if (g_file_test(filename, G_FILE_TEST_IS_DIR)) {
			gchar *new_filename;
			new_filename = g_strconcat(filename,
						   G_DIR_SEPARATOR_S, NULL);
			g_free(filename);
			filename = new_filename;
		}

		files = g_list_prepend(files, filename);
	}

	g_free(dirname);
	g_dir_close(dir);

	completion = g_completion_new(NULL);
	g_completion_add_items(completion, files);

	matching = g_completion_complete(completion, filename, &new_prefix);
	if (new_prefix && strlen(new_prefix) > strlen(filename))
		insert = g_strdup(new_prefix + strlen(filename));
	g_free(new_prefix);

	if (!insert && g_list_length(matching) > 1) {
		matching = g_list_sort(matching, (GCompareFunc)g_strcmp0);

		for (GList *file = g_list_first(matching);
		     file != NULL;
		     file = g_list_next(file)) {
			Interface::PopupFileType type;
			bool in_buffer = false;

			if (filename_is_dir((gchar *)file->data)) {
				type = Interface::POPUP_DIRECTORY;
			} else {
				type = Interface::POPUP_FILE;
				/* FIXME: inefficient */
				in_buffer = ring.find((gchar *)file->data);
			}

			interface.popup_add_filename(type, (gchar *)file->data,
						     in_buffer);
		}

		interface.popup_show();
	} else if (g_list_length(matching) == 1 &&
		   !filename_is_dir((gchar *)g_list_first(matching)->data)) {
		gchar *new_insert;

		new_insert = g_strconcat(insert ? : "",
					 (gchar []){completed, '\0'}, NULL);
		g_free(insert);
		insert = new_insert;
	}

	g_completion_free(completion);

	for (GList *file = g_list_first(files); file; file = g_list_next(file))
		g_free(file->data);
	g_list_free(files);

	return insert;
}

/*
 * Auxiliary functions
 */

static const gchar *
last_occurrence(const gchar *str, const gchar *chars)
{
	while (*chars)
		str = strrchr(str, *chars++) ? : str;

	return str;
}

static inline gboolean
filename_is_dir(const gchar *filename)
{
	return g_str_has_suffix(filename, G_DIR_SEPARATOR_S);
}
