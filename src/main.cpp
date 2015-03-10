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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <new>

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
	tecoInt ed = 0;
}

static gchar *eval_macro = NULL;
static gchar *mung_file = NULL;
static gboolean mung_profile = TRUE;

sig_atomic_t sigint_occurred = FALSE;

extern "C" {
static gpointer g_malloc_exception(gsize n_bytes);
static gpointer g_calloc_exception(gsize n_blocks, gsize n_block_bytes);
static gpointer g_realloc_exception(gpointer mem, gsize n_bytes);

static void sigint_handler(int signal);
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

static inline gchar *
get_default_config_path(const gchar *program)
{
	return g_strdup(g_getenv("HOME") ? : g_get_home_dir());
}

#else

static inline gchar *
get_default_config_path(const gchar *program)
{
	return g_strdup(g_get_user_config_dir());
}

#endif

static inline void
process_options(int &argc, char **&argv)
{
	static const GOptionEntry option_entries[] = {
		{"eval", 'e', 0, G_OPTION_ARG_STRING, &eval_macro,
		 "Evaluate macro", "macro"},
		{"mung", 'm', 0, G_OPTION_ARG_FILENAME, &mung_file,
		 "Mung file instead of "
		 "$SCITECOCONFIG" G_DIR_SEPARATOR_S INI_FILE, "file"},
		{"no-profile", 0, G_OPTION_FLAG_REVERSE,
		 G_OPTION_ARG_NONE, &mung_profile,
		 "Do not mung "
		 "$SCITECOCONFIG" G_DIR_SEPARATOR_S INI_FILE
		 " even if it exists"},
		{NULL}
	};

	GError *gerror = NULL;

	GOptionContext	*options;
	GOptionGroup	*interface_group = interface.get_options();

	options = g_option_context_new("[--] [ARGUMENT...]");

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

	if (!g_option_context_parse(options, &argc, &argv, &gerror)) {
		g_fprintf(stderr, "Option parsing failed: %s\n",
			  gerror->message);
		g_error_free(gerror);
		exit(EXIT_FAILURE);
	}

	g_option_context_free(options);

	if (mung_file) {
		if (!g_file_test(mung_file, G_FILE_TEST_IS_REGULAR)) {
			g_fprintf(stderr, "Cannot mung \"%s\". File does not exist!\n",
				  mung_file);
			exit(EXIT_FAILURE);
		}
	}

	/* remaining arguments, are arguments to the interface */
}

static inline void
initialize_environment(const gchar *program)
{
	gchar *default_configpath, *abs_path;
	gchar **env;

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

	env = g_listenv();

	for (gchar **key = env; *key; key++) {
		gchar name[1 + strlen(*key) + 1];
		QRegister *reg;

		name[0] = '$';
		strcpy(name + 1, *key);

		reg = QRegisters::globals.insert(name);
		reg->set_string(g_getenv(*key));
	}

	g_strfreev(env);
}

/*
 * Callbacks
 */

class g_bad_alloc : public std::bad_alloc {
public:
	const char *
	what() const throw()
	{
		return "glib allocation";
	}
};

static gpointer
g_malloc_exception(gsize n_bytes)
{
	gpointer p = malloc(n_bytes);

	if (!p)
		throw g_bad_alloc();
	return p;
}

static gpointer
g_calloc_exception(gsize n_blocks, gsize n_block_bytes)
{
	gpointer p = calloc(n_blocks, n_block_bytes);

	if (!p)
		throw g_bad_alloc();
	return p;
}

static gpointer
g_realloc_exception(gpointer mem, gsize n_bytes)
{
	gpointer p = realloc(mem, n_bytes);

	if (!p)
		throw g_bad_alloc();
	return p;
}

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

	static GMemVTable vtable = {
		g_malloc_exception,	/* malloc */
		g_realloc_exception,	/* realloc */
		free,			/* free */
		g_calloc_exception,	/* calloc */
		malloc,			/* try_malloc */
		realloc			/* try_realloc */
	};

#ifdef DEBUG_PAUSE
	/* Windows debugging hack (see above) */
	system("pause");
#endif

	signal(SIGINT, sigint_handler);

	g_mem_set_vtable(&vtable);

	process_options(argc, argv);
	interface.main(argc, argv);
	/* remaining arguments are arguments to the munged file */

	/*
	 * QRegister view must be initialized only now
	 * (e.g. after Curses/GTK initialization).
	 */
	QRegisters::view.initialize();

	/* search string and status register */
	QRegisters::globals.insert("_");
	/* replacement string register */
	QRegisters::globals.insert("-");
	/* current buffer name and number ("*") */
	QRegisters::globals.insert(new QRegisterBufferInfo());
	/* environment defaults and registers */
	initialize_environment(argv[0]);

	QRegisters::locals = &local_qregs;

	ring.edit((const gchar *)NULL);

	/* add remaining arguments to unnamed buffer */
	for (int i = 1; i < argc; i++) {
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
			}
			QRegisters::hook(QRegisters::HOOK_QUIT);
			exit(EXIT_SUCCESS);
		}

		if (!mung_file && mung_profile)
			mung_file = g_build_filename(g_getenv("SCITECOCONFIG"),
			                             INI_FILE, NIL);

		if (mung_file &&
		    g_file_test(mung_file, G_FILE_TEST_IS_REGULAR)) {
			Execute::file(mung_file, false);

			/* FIXME: make quit immediate in batch/macro mode (non-UNDO)? */
			if (quit_requested) {
				QRegisters::hook(QRegisters::HOOK_QUIT);
				exit(EXIT_SUCCESS);
			}
		}
		g_free(mung_file);
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

	return 0;
}
