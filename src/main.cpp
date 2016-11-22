/*
 * Copyright (C) 2012-2016 Robin Haberkorn
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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>

#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>

#include "sciteco.h"
#include "cmdline.h"
#include "interface.h"
#include "ioview.h"
#include "parser.h"
#include "goto.h"
#include "qregisters.h"
#include "ring.h"
#include "undo.h"
#include "error.h"

/*
 * Define this to pause the program at the beginning
 * of main() (Windows only).
 * This is a useful hack on Windows, where gdbserver
 * sometimes refuses to start SciTECO but attaches
 * to a running process just fine.
 */
//#define DEBUG_PAUSE

namespace SciTECO {

#define INI_FILE ".teco_ini"

/*
 * defining the global objects here ensures
 * a ctor/dtor order without depending on the
 * GCC init_priority() attribute
 */
InterfaceCurrent interface;
IOView QRegisters::view;

/*
 * Scintilla will be initialized after these
 * ctors (in main()), but dtors are guaranteed
 * to be executed before Scintilla's
 * destruction
 */
QRegisterTable QRegisters::globals;
Ring ring;

namespace Flags {
	tecoInt ed = ED_AUTOEOL;
}

static gchar *eval_macro = NULL;
static gboolean mung_file = FALSE;
static gboolean mung_profile = TRUE;

sig_atomic_t sigint_occurred = FALSE;

extern "C" {

static void sigint_handler(int signal);

} /* extern "C" */

#if defined(G_OS_UNIX) || defined(G_OS_HAIKU)

void
interrupt(void)
{
	/*
	 * This sends SIGINT to the entire process group,
	 * which makes sure that subprocesses are signalled,
	 * even when called from the wrong thread.
	 */
	if (kill(0, SIGINT))
		sigint_occurred = TRUE;
}

#else /* !G_OS_UNIX && !G_OS_HAIKU */

void
interrupt(void)
{
	if (raise(SIGINT))
		sigint_occurred = TRUE;
}

#endif

const gchar *
get_eol_seq(gint eol_mode)
{
	switch (eol_mode) {
	case SC_EOL_CRLF:
		return "\r\n";
	case SC_EOL_CR:
		return "\r";
	case SC_EOL_LF:
	default:
		return "\n";
	}
}

#ifdef G_OS_WIN32

/*
 * Keep program self-contained under Windows
 * Look for config files (profile and session),
 * as well as standard library macros in the
 * program's directory.
 */
static inline gchar *
get_default_config_path(const gchar *program)
{
	return g_path_get_dirname(program);
}

#elif defined(G_OS_UNIX)

/*
 * NOTE: We explicitly do not handle
 * Haiku like UNIX here, since it appears to
 * be uncommon on Haiku to clutter the HOME directory
 * with config files.
 */
static inline gchar *
get_default_config_path(const gchar *program)
{
	return g_strdup(g_getenv("HOME"));
}

#else

static inline gchar *
get_default_config_path(const gchar *program)
{
	return g_strdup(g_get_user_config_dir());
}

#endif

static inline gchar *
process_options(int &argc, char **&argv)
{
	static const GOptionEntry option_entries[] = {
		{"eval", 'e', 0, G_OPTION_ARG_STRING, &eval_macro,
		 "Evaluate macro", "macro"},
		{"mung", 'm', 0, G_OPTION_ARG_NONE, &mung_file,
		 "Mung script file (first non-option argument) instead of "
		 "$SCITECOCONFIG" G_DIR_SEPARATOR_S INI_FILE},
		{"no-profile", 0, G_OPTION_FLAG_REVERSE,
		 G_OPTION_ARG_NONE, &mung_profile,
		 "Do not mung "
		 "$SCITECOCONFIG" G_DIR_SEPARATOR_S INI_FILE " "
		 "even if it exists"},
		{NULL}
	};

	gchar *mung_filename = NULL;

	GError *gerror = NULL;

	GOptionContext	*options;
	GOptionGroup	*interface_group = interface.get_options();

	options = g_option_context_new("[--] [SCRIPT] [ARGUMENT...]");

	g_option_context_set_summary(
		options,
		PACKAGE_STRING " -- Scintilla-based Text Editor and COrrector"
	);
	g_option_context_set_description(
		options,
		"Bug reports should go to <" PACKAGE_BUGREPORT "> or "
		"<" PACKAGE_URL_DEV ">."
	);

	g_option_context_add_main_entries(options, option_entries, NULL);
	if (interface_group)
		g_option_context_add_group(options, interface_group);

#if GLIB_CHECK_VERSION(2,44,0)
	/*
	 * If possible we parse in POSIX mode, which means that
	 * the first non-option argument terminates option parsing.
	 * SciTECO considers all non-option arguments to be script
	 * arguments and it makes little sense to mix script arguments
	 * with SciTECO options, so this lets the user avoid "--"
	 * in many situations.
	 * It is also strictly required to make hash-bang lines like
	 * #!/usr/bin/sciteco -m
	 * work (see sciteco(1)).
	 */
	g_option_context_set_strict_posix(options, TRUE);
#endif

	if (!g_option_context_parse(options, &argc, &argv, &gerror)) {
		g_fprintf(stderr, "Option parsing failed: %s\n",
			  gerror->message);
		g_error_free(gerror);
		exit(EXIT_FAILURE);
	}

	g_option_context_free(options);

	/*
	 * GOption will NOT remove "--" if followed by an
	 * option-argument, which may interfer with scripts
	 * doing their own option handling and interpreting "--".
	 *
	 * NOTE: This is still true if we're parsing in GNU-mode
	 * and "--" is not the first non-option argument as in
	 * sciteco foo -- -C bar.
	 */
	if (argc >= 2 && !strcmp(argv[1], "--")) {
		argv[1] = argv[0];
		argv++;
		argc--;
	}

	if (mung_file) {
		if (argc < 2) {
			g_fprintf(stderr, "Script to mung expected!\n");
			exit(EXIT_FAILURE);
		}

		if (!g_file_test(argv[1], G_FILE_TEST_IS_REGULAR)) {
			g_fprintf(stderr, "Cannot mung \"%s\". File does not exist!\n",
				  argv[1]);
			exit(EXIT_FAILURE);
		}

		mung_filename = g_strdup(argv[1]);

		argv[1] = argv[0];
		argv++;
		argc--;
	}

	return mung_filename;
}

static inline void
initialize_environment(const gchar *program)
{
	gchar *default_configpath, *abs_path;

	/*
	 * Initialize some "special" environment variables.
	 * For ease of use and because there are no threads yet,
	 * we modify the process environment directly.
	 * Later it is imported into the global Q-Register table
	 * and the process environment should no longer be accessed
	 * directly.
	 *
	 * Initialize and canonicalize $HOME.
	 * Therefore we can refer to $HOME as the
	 * current user's home directory on any platform
	 * and it can be re-configured even though g_get_home_dir()
	 * evaluates $HOME only beginning with glib v2.36.
	 */
	g_setenv("HOME", g_get_home_dir(), FALSE);
	abs_path = get_absolute_path(g_getenv("HOME"));
	g_setenv("HOME", abs_path, TRUE);
	g_free(abs_path);

#ifdef G_OS_WIN32
	g_setenv("COMSPEC", "cmd.exe", FALSE);
#elif defined(G_OS_UNIX) || defined(G_OS_HAIKU)
	g_setenv("SHELL", "/bin/sh", FALSE);
#endif

	/*
	 * Initialize $SCITECOCONFIG and $SCITECOPATH
	 */
	default_configpath = get_default_config_path(program);
	g_setenv("SCITECOCONFIG", default_configpath, FALSE);
#ifdef G_OS_WIN32
	gchar *default_scitecopath;
	default_scitecopath = g_build_filename(default_configpath, "lib", NIL);
	g_setenv("SCITECOPATH", default_scitecopath, FALSE);
	g_free(default_scitecopath);
#else
	g_setenv("SCITECOPATH", SCITECOLIBDIR, FALSE);
#endif
	g_free(default_configpath);

	/*
	 * $SCITECOCONFIG and $SCITECOPATH may still be relative.
	 * They are canonicalized, so macros can use them even
	 * if the current working directory changes.
	 */
	abs_path = get_absolute_path(g_getenv("SCITECOCONFIG"));
	g_setenv("SCITECOCONFIG", abs_path, TRUE);
	g_free(abs_path);
	abs_path = get_absolute_path(g_getenv("SCITECOPATH"));
	g_setenv("SCITECOPATH", abs_path, TRUE);
	g_free(abs_path);

	/*
	 * Import process environment into global Q-Register
	 * table. While it is safe to use g_setenv() early
	 * on at startup, it might be problematic later on
	 * (e.g. it's non-thread-safe).
	 * Therefore the environment registers in the global
	 * table should be used from now on to set and get
	 * environment variables.
	 * When spawning external processes that should inherit
	 * the environment variables, the environment should
	 * be exported via QRegisters::globals.get_environ().
	 */
	QRegisters::globals.set_environ();
}

/*
 * Callbacks
 */

static void
sigint_handler(int signal)
{
	sigint_occurred = TRUE;
}

} /* namespace SciTECO */

/*
 * main() must be defined in the root
 * namespace, so we import the "SciTECO"
 * namespace. We have no more declarations
 * to make in the "SciTECO" namespace.
 */
using namespace SciTECO;

int
main(int argc, char **argv)
{
	static GotoTable	cmdline_goto_table;
	static QRegisterTable	local_qregs;

	gchar *mung_filename;

#ifdef DEBUG_PAUSE
	/* Windows debugging hack (see above) */
	system("pause");
#endif

	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);

	mung_filename = process_options(argc, argv);
	/*
	 * All remaining arguments in argv are arguments
	 * to the macro or munged file.
	 */
	interface.init();

	/*
	 * QRegister view must be initialized only now
	 * (e.g. after Curses/GTK initialization).
	 */
	QRegisters::view.initialize();

	/* the default registers (A-Z and 0-9) */
	QRegisters::globals.insert_defaults();
	/* search string and status register */
	QRegisters::globals.insert("_");
	/* replacement string register */
	QRegisters::globals.insert("-");
	/* current buffer name and number ("*") */
	QRegisters::globals.insert(new QRegisterBufferInfo());
	/* current working directory ("$") */
	QRegisters::globals.insert(new QRegisterWorkingDir());
	/* environment defaults and registers */
	initialize_environment(argv[0]);

	/* the default registers (A-Z and 0-9) */
	local_qregs.insert_defaults();
	QRegisters::locals = &local_qregs;

	ring.edit((const gchar *)NULL);

	/* add remaining arguments to unnamed buffer */
	for (gint i = 1; i < argc; i++) {
		/*
		 * FIXME: arguments may contain line-feeds.
		 * Once SciTECO is 8-byte clear, we can add the
		 * command-line params null-terminated.
		 */
		interface.ssm(SCI_APPENDTEXT, strlen(argv[i]), (sptr_t)argv[i]);
		interface.ssm(SCI_APPENDTEXT, 1, (sptr_t)"\n");
	}

	/*
	 * Execute macro or mung file
	 */
	try {
		if (eval_macro) {
			try {
				Execute::macro(eval_macro, false);
			} catch (Error &error) {
				error.add_frame(new Error::ToplevelFrame());
				throw; /* forward */
			} catch (Quit) {
				/*
				 * ^C invoked, quit hook should still
				 * be executed.
				 */
			}
			QRegisters::hook(QRegisters::HOOK_QUIT);
			exit(EXIT_SUCCESS);
		}

		if (!mung_filename && mung_profile)
			/* NOTE: Still safe to use g_getenv() */
			mung_filename = g_build_filename(g_getenv("SCITECOCONFIG"),
			                                 INI_FILE, NIL);

		if (mung_filename &&
		    g_file_test(mung_filename, G_FILE_TEST_IS_REGULAR)) {
			try {
				Execute::file(mung_filename, false);
			} catch (Quit) {
				/*
				 * ^C invoked, quit hook should still
				 * be executed.
				 */
			}

			if (quit_requested) {
				QRegisters::hook(QRegisters::HOOK_QUIT);
				exit(EXIT_SUCCESS);
			}
		}
	} catch (Error &error) {
		error.display_full();
		exit(EXIT_FAILURE);
	} catch (...) {
		exit(EXIT_FAILURE);
	}

	/*
	 * If munged file didn't quit, switch into interactive mode
	 */
	/* commandline replacement string register */
	QRegisters::globals.insert(CTL_KEY_ESC_STR);

	Goto::table = &cmdline_goto_table;
	undo.enabled = true;
	ring.set_scintilla_undo(true);
	QRegisters::view.set_scintilla_undo(true);

	interface.event_loop();

	/*
	 * Ordinary application termination:
	 * Interface is shut down, so we are
	 * in non-interactive mode again.
	 */
	undo.enabled = false;
	undo.clear();
	/* also empties all Scintilla undo buffers */
	ring.set_scintilla_undo(false);
	QRegisters::view.set_scintilla_undo(false);

	try {
		QRegisters::hook(QRegisters::HOOK_QUIT);
	} catch (Error &error) {
		error.display_full();
		exit(EXIT_FAILURE);
	}

	g_free(mung_filename);
	return 0;
}
