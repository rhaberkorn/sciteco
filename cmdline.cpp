#include <string.h>

#include <glib.h>
#include <glib/gprintf.h>

#include "sciteco.h"
#include "parser.h"
#include "undo.h"

static gchar *macro_echo(const gchar *macro, const gchar *prefix = "");

gchar *cmdline = NULL;

void
cmdline_keypress(gchar key)
{
	gchar insert[255] = "";
	gint old_cmdline_len = 0;
	gchar *echo;

	/*
	 * Process immediate editing commands
	 */
	switch (key) {
	case '\b':
		if (!cmdline || !*cmdline)
			break;
		old_cmdline_len = strlen(cmdline);

		undo.pop(old_cmdline_len);
		cmdline[old_cmdline_len-1] = '\0';
		macro_pc--;
		break;
	default:
		insert[0] = key;
		insert[1] = '\0';
	}

	/*
	 * Parse/execute characters
	 */
	if (cmdline) {
		old_cmdline_len = strlen(cmdline);
		cmdline = (gchar *)g_realloc(cmdline, old_cmdline_len +
						      strlen(insert) + 1);
	} else {
		cmdline = (gchar *)g_malloc(2);
		*cmdline = '\0';
	}

	for (gchar *p = insert; *p; p++) {
		strcat(cmdline, (gchar[]){key, '\0'});

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
