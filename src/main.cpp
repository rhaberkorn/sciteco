/*
 * Copyright (C) 2012-2013 Robin Haberkorn
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

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>

#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>

#include <Scintilla.h>
#include <SciLexer.h>

#include "sciteco.h"
#include "cmdline.h"
#include "interface.h"
#include "parser.h"
#include "goto.h"
#include "qregisters.h"
#include "ring.h"
#include "undo.h"

#ifdef G_OS_UNIX
#define INI_FILE ".teco_ini"
#else
#define INI_FILE "teco.ini"
#endif

namespace Flags {
	gint64 ed = 0;
}

static gchar *mung_file = NULL;

sig_atomic_t sigint_occurred = FALSE;

extern "C" {
static void sigint_handler(int signal);
}

void
Interface::stdio_vmsg(MessageType type, const gchar *fmt, va_list ap)
{
	gchar buf[255];

	g_vsnprintf(buf, sizeof(buf), fmt, ap);

	switch (type) {
	case MSG_USER:
		g_printf("%s\n", buf);
		break;
	case MSG_INFO:
		g_printf("Info: %s\n", buf);
		break;
	case MSG_WARNING:
		g_fprintf(stderr, "Warning: %s\n", buf);
		break;
	case MSG_ERROR:
		g_fprintf(stderr, "Error: %s\n", buf);
		break;
	}
}

void
Interface::process_notify(SCNotification *notify)
{
#ifdef DEBUG
	g_printf("SCINTILLA NOTIFY: code=%d\n", notify->nmhdr.code);
#endif
}

#ifdef G_OS_WIN32

/*
 * Keep program self-contained under Windows
 * (look for profile in program's directory)
 */
static inline gchar *
get_teco_ini(const gchar *program)
{
	gchar *bin_dir = g_path_get_dirname(program);
	gchar *ini = g_build_filename(bin_dir, INI_FILE, NULL);
	g_free(bin_dir);
	return ini;
}

#else

static inline gchar *
get_teco_ini(const gchar *program __attribute__((unused)))
{
	const gchar *home;

#ifdef G_OS_UNIX
	home = g_get_home_dir();
#else
	home = g_get_user_config_dir();
#endif
	return g_build_filename(home, INI_FILE, NULL);
}

#endif /* !G_OS_WIN32 */

static inline void
process_options(int &argc, char **&argv)
{
	static const GOptionEntry option_entries[] = {
		{"mung", 'm', 0, G_OPTION_ARG_FILENAME, &mung_file,
		 "Mung file instead of " INI_FILE, "filename"},
		{NULL}
	};

	GOptionContext	*options;
	GOptionGroup	*interface_group = interface.get_options();

	options = g_option_context_new("- " PACKAGE_STRING);

	g_option_context_add_main_entries(options, option_entries, NULL);
	if (interface_group)
		g_option_context_add_group(options, interface_group);

	if (!g_option_context_parse(options, &argc, &argv, NULL)) {
		g_printf("Option parsing failed!\n");
		exit(EXIT_FAILURE);
	}

	g_option_context_free(options);

	if (mung_file) {
		if (!g_file_test(mung_file, G_FILE_TEST_IS_REGULAR)) {
			g_printf("Cannot mung \"%s\". File does not exist!\n",
				 mung_file);
			exit(EXIT_FAILURE);
		}
	} else {
		mung_file = get_teco_ini(argv[0]);
	}

	interface.parse_args(argc, argv);

	/* remaining arguments, are arguments to the munged file */
}

int
main(int argc, char **argv)
{
	static GotoTable	cmdline_goto_table;
	static QRegisterTable	local_qregs;

	signal(SIGINT, sigint_handler);

	process_options(argc, argv);

	interface.ssm(SCI_SETCARETSTYLE, CARETSTYLE_BLOCK);
	interface.ssm(SCI_SETCARETFORE, 0xFFFFFF);

	/*
	 * FIXME: Default styles should probably be set interface-based
	 * (system defaults) and be changeable by TECO macros
	 */
	interface.ssm(SCI_STYLESETFORE, STYLE_DEFAULT, 0xFFFFFF);
	interface.ssm(SCI_STYLESETBACK, STYLE_DEFAULT, 0x000000);
	interface.ssm(SCI_STYLESETFONT, STYLE_DEFAULT, (sptr_t)"Courier");
	interface.ssm(SCI_STYLECLEARALL);

	QRegisters::globals.initialize();
	/* search string and status register */
	QRegisters::globals.initialize("_");
	/* replacement string register */
	QRegisters::globals.initialize("-");
	/* current buffer name and number ("*") */
	QRegisters::globals.insert(new QRegisterBufferInfo());

	local_qregs.initialize();
	QRegisters::locals = &local_qregs;

	ring.edit((const gchar *)NULL);

	/* add remaining arguments to unnamed buffer */
	for (int i = 1; i < argc; i++) {
		interface.ssm(SCI_APPENDTEXT, strlen(argv[i]), (sptr_t)argv[i]);
		interface.ssm(SCI_APPENDTEXT, 1, (sptr_t)"\n");
	}

	if (g_file_test(mung_file, G_FILE_TEST_IS_REGULAR)) {
		if (!Execute::file(mung_file, false))
			exit(EXIT_FAILURE);

		/* FIXME: make quit immediate in batch/macro mode (non-UNDO)? */
		if (quit_requested) {
			/* FIXME */
			exit(EXIT_SUCCESS);
		}
	}
	g_free(mung_file);

	Goto::table = &cmdline_goto_table;
	interface.ssm(SCI_EMPTYUNDOBUFFER);
	undo.enabled = true;

	interface.event_loop();

	return 0;
}

/*
 * Callbacks
 */

static void
sigint_handler(int signal __attribute__((unused)))
{
	sigint_occurred = TRUE;
}
