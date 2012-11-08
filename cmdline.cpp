#include <string.h>
#include <stdlib.h>

#include <glib.h>
#include <glib/gprintf.h>

#include "sciteco.h"
#include "parser.h"
#include "goto.h"
#include "undo.h"

static inline const gchar *process_edit_cmd(gchar key);
static gchar *macro_echo(const gchar *macro, const gchar *prefix = "");

gchar *cmdline = NULL;

bool quit_requested = false;

void
cmdline_keypress(gchar key)
{
	const gchar *insert;
	gint old_cmdline_len = 0;
	gchar *echo;

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

		if (!macro_execute(cmdline)) {
			cmdline[old_cmdline_len] = '\0';
			break;
		}
	}

	/*
	 * Echo command line
	 */
	echo = macro_echo(cmdline, "*");
	cmdline_display(echo);
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

	case '\x1B':
		if (cmdline && cmdline[cmdline_len - 1] == '\x1B') {
			if (quit_requested) {
				/* FIXME */
				exit(EXIT_SUCCESS);
			}

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
macro_echo(const gchar *macro, const gchar *prefix)
{
	gchar *result, *rp;

	if (!macro)
		return g_strdup(prefix);

	result = (gchar *)g_malloc(strlen(prefix) + strlen(macro)*5 + 1);
	rp = g_stpcpy(result, prefix);

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
