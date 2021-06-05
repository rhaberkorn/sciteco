/*
 * Copyright (C) 2012-2021 Robin Haberkorn
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
#include "file-utils.h"
#include "cmdline.h"
#include "interface.h"
#include "parser.h"
#include "goto.h"
#include "qreg.h"
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

#define INI_FILE ".teco_ini"

teco_int_t teco_ed = TECO_ED_AUTOEOL;

volatile sig_atomic_t teco_sigint_occurred = FALSE;

#ifdef G_OS_UNIX

void
teco_interrupt(void)
{
	/*
	 * This sends SIGINT to the entire process group,
	 * which makes sure that subprocesses are signalled,
	 * even when called from the wrong thread.
	 */
	if (kill(0, SIGINT))
		teco_sigint_occurred = TRUE;
}

#else /* !G_OS_UNIX */

void
teco_interrupt(void)
{
	if (raise(SIGINT))
		teco_sigint_occurred = TRUE;
}

#endif

/*
 * FIXME: Move this into file-utils.c?
 */
#ifdef G_OS_WIN32

/*
 * Keep program self-contained under Windows
 * Look for config files (profile and session),
 * as well as standard library macros in the
 * program's directory.
 */
static inline gchar *
teco_get_default_config_path(const gchar *program)
{
	return g_path_get_dirname(program);
}

#elif defined(G_OS_UNIX) && !defined(__HAIKU__)

static inline gchar *
teco_get_default_config_path(const gchar *program)
{
	return g_strdup(g_get_home_dir());
}

#else /* !G_OS_WIN32 && (!G_OS_UNIX || __HAIKU__) */

/*
 * NOTE: We explicitly do not handle
 * Haiku like UNIX, since it appears to
 * be uncommon on Haiku to clutter the $HOME directory
 * with config files.
 */
static inline gchar *
teco_get_default_config_path(const gchar *program)
{
	return g_strdup(g_get_user_config_dir());
}

#endif

static gchar *teco_eval_macro = NULL;
static gboolean teco_mung_file = FALSE;
static gboolean teco_mung_profile = TRUE;

static gchar *
teco_process_options(gint *argc, gchar ***argv)
{
	static const GOptionEntry option_entries[] = {
		{"eval", 'e', 0, G_OPTION_ARG_STRING, &teco_eval_macro,
		 "Evaluate macro", "macro"},
		{"mung", 'm', 0, G_OPTION_ARG_NONE, &teco_mung_file,
		 "Mung script file (first non-option argument) instead of "
		 "$SCITECOCONFIG" G_DIR_SEPARATOR_S INI_FILE},
		{"no-profile", 0, G_OPTION_FLAG_REVERSE,
		 G_OPTION_ARG_NONE, &teco_mung_profile,
		 "Do not mung "
		 "$SCITECOCONFIG" G_DIR_SEPARATOR_S INI_FILE " "
		 "even if it exists"},
		{NULL}
	};

	g_autoptr(GError) error = NULL;

	g_autoptr(GOptionContext) options = g_option_context_new("[--] [SCRIPT] [ARGUMENT...]");

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

	GOptionGroup *interface_group = teco_interface_get_options();
	if (interface_group)
		g_option_context_add_group(options, interface_group);

	/*
	 * We parse in POSIX mode, which means that
	 * the first non-option argument terminates option parsing.
	 * SciTECO considers all non-option arguments to be script
	 * arguments and it makes little sense to mix script arguments
	 * with SciTECO options, so this lets the user avoid "--"
	 * in many situations.
	 * It is also strictly required to make hash-bang lines like
	 * #!/usr/bin/sciteco -m
	 * work.
	 */
	g_option_context_set_strict_posix(options, TRUE);

	if (!g_option_context_parse(options, argc, argv, &error)) {
		g_fprintf(stderr, "Option parsing failed: %s\n",
			  error->message);
		exit(EXIT_FAILURE);
	}

	/*
	 * GOption will NOT remove "--" if followed by an
	 * option-argument, which may interfer with scripts
	 * doing their own option handling and interpreting "--".
	 *
	 * NOTE: This is still true if we're parsing in GNU-mode
	 * and "--" is not the first non-option argument as in
	 * sciteco foo -- -C bar.
	 */
	if (*argc >= 2 && !strcmp((*argv)[1], "--")) {
		(*argv)[1] = (*argv)[0];
		(*argv)++;
		(*argc)--;
	}

	gchar *mung_filename = NULL;

	if (teco_mung_file) {
		if (*argc < 2) {
			g_fprintf(stderr, "Script to mung expected!\n");
			exit(EXIT_FAILURE);
		}

		if (!g_file_test((*argv)[1], G_FILE_TEST_IS_REGULAR)) {
			g_fprintf(stderr, "Cannot mung \"%s\". File does not exist!\n",
			          (*argv)[1]);
			exit(EXIT_FAILURE);
		}

		mung_filename = g_strdup((*argv)[1]);

		(*argv)[1] = (*argv)[0];
		(*argv)++;
		(*argc)--;
	}

	return mung_filename;
}

static void
teco_initialize_environment(const gchar *program)
{
	g_autoptr(GError) error = NULL;
	gchar *abs_path;

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
	 * and you can even start SciTECO with $HOME set to a relative
	 * path (sometimes useful for testing).
	 */
	g_setenv("HOME", g_get_home_dir(), FALSE);
	abs_path = teco_file_get_absolute_path(g_getenv("HOME"));
	g_setenv("HOME", abs_path, TRUE);
	g_free(abs_path);

#ifdef G_OS_WIN32
	/*
	 * NOTE: Environment variables are case-insensitive on Windows
	 * and there may be either a $COMSPEC or $ComSpec variable.
	 * By unsetting and resetting $COMSPEC, we make sure that
	 * it exists with defined case in the environment and therefore
	 * as a Q-Register.
	 */
	g_autofree gchar *comspec = g_strdup(g_getenv("COMSPEC") ? : "cmd.exe");
	g_unsetenv("COMSPEC");
	g_setenv("COMSPEC", comspec, TRUE);
#elif defined(G_OS_UNIX)
	g_setenv("SHELL", "/bin/sh", FALSE);
#endif

	/*
	 * Initialize $SCITECOCONFIG and $SCITECOPATH
	 */
	g_autofree gchar *default_configpath = teco_get_default_config_path(program);
	g_setenv("SCITECOCONFIG", default_configpath, FALSE);
#ifdef G_OS_WIN32
	g_autofree gchar *default_scitecopath = g_build_filename(default_configpath, "lib", NULL);
	g_setenv("SCITECOPATH", default_scitecopath, FALSE);
#else
	g_setenv("SCITECOPATH", SCITECOLIBDIR, FALSE);
#endif

	/*
	 * $SCITECOCONFIG and $SCITECOPATH may still be relative.
	 * They are canonicalized, so macros can use them even
	 * if the current working directory changes.
	 */
	abs_path = teco_file_get_absolute_path(g_getenv("SCITECOCONFIG"));
	g_setenv("SCITECOCONFIG", abs_path, TRUE);
	g_free(abs_path);
	abs_path = teco_file_get_absolute_path(g_getenv("SCITECOPATH"));
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
	if (!teco_qreg_table_set_environ(&teco_qreg_table_globals, &error)) {
		g_fprintf(stderr, "Error intializing environment: %s\n",
		          error->message);
		exit(EXIT_FAILURE);
	}
}

/*
 * Callbacks
 */

static void
teco_sigint_handler(int signal)
{
	teco_sigint_occurred = TRUE;
}

int
main(int argc, char **argv)
{
	g_autoptr(GError) error = NULL;

#ifdef DEBUG_PAUSE
	/* Windows debugging hack (see above) */
	system("pause");
#endif

	signal(SIGINT, teco_sigint_handler);
	signal(SIGTERM, teco_sigint_handler);

	g_autofree gchar *mung_filename = teco_process_options(&argc, &argv);
	/*
	 * All remaining arguments in argv are arguments
	 * to the macro or munged file.
	 */

	/*
	 * Theoretically, QReg tables should only be initialized
	 * after the interface, since they contain Scintilla documents.
	 * However, this would prevent the inialization of clipboard QRegs
	 * in teco_interface_init() and those should be available in batch mode
	 * as well.
	 * As long as the string parts are not accessed, that should be OK.
	 *
	 * FIXME: Perhaps it would be better to introduce something like
	 * teco_interface_init_clipboard()?
	 */
	teco_qreg_table_init(&teco_qreg_table_globals, TRUE);

	teco_interface_init();

	/*
	 * QRegister view must be initialized only now
	 * (e.g. after Curses/GTK initialization).
	 */
	teco_qreg_view = teco_view_new();
	teco_view_setup(teco_qreg_view);

	/* search string and status register */
	teco_qreg_table_insert(&teco_qreg_table_globals, teco_qreg_plain_new("_", 1));
	/* replacement string register */
	teco_qreg_table_insert(&teco_qreg_table_globals, teco_qreg_plain_new("-", 1));
	/* current buffer name and number ("*") */
	teco_qreg_table_insert(&teco_qreg_table_globals, teco_qreg_bufferinfo_new());
	/* current working directory ("$") */
	teco_qreg_table_insert(&teco_qreg_table_globals, teco_qreg_workingdir_new());
	/* environment defaults and registers */
	teco_initialize_environment(argv[0]);

	teco_qreg_table_t local_qregs;
	teco_qreg_table_init(&local_qregs, TRUE);

	if (!teco_ring_edit_by_name(NULL, &error)) {
		g_fprintf(stderr, "Error editing unnamed file: %s\n",
		          error->message);
		exit(EXIT_FAILURE);
	}

	/*
	 * Add remaining arguments to unnamed buffer.
	 *
	 * FIXME: This is not really robust since filenames may contain linefeeds.
	 * Also, the Unnamed Buffer should be kept empty for piping.
	 * Therefore, it would be best to store the arguments in Q-Regs, e.g. $0,$1,$2...
	 */
	for (gint i = 1; i < argc; i++) {
		teco_interface_ssm(SCI_APPENDTEXT, strlen(argv[i]), (sptr_t)argv[i]);
		teco_interface_ssm(SCI_APPENDTEXT, 1, (sptr_t)"\n");
	}

	/*
	 * Execute macro or mung file
	 */
	if (teco_eval_macro) {
		if (!teco_execute_macro(teco_eval_macro, strlen(teco_eval_macro),
		                        &local_qregs, &error) &&
		    !g_error_matches(error, TECO_ERROR, TECO_ERROR_QUIT)) {
			teco_error_add_frame_toplevel();
			goto error;
		}
		if (!teco_ed_hook(TECO_ED_HOOK_QUIT, &error))
			goto error;
		goto cleanup;
	}

	if (!mung_filename && teco_mung_profile)
		/* NOTE: Still safe to use g_getenv() */
		mung_filename = g_build_filename(g_getenv("SCITECOCONFIG"), INI_FILE, NULL);

	if (mung_filename && g_file_test(mung_filename, G_FILE_TEST_IS_REGULAR)) {
		if (!teco_execute_file(mung_filename, &local_qregs, &error) &&
		    !g_error_matches(error, TECO_ERROR, TECO_ERROR_QUIT))
			goto error;

		if (teco_quit_requested) {
			if (!teco_ed_hook(TECO_ED_HOOK_QUIT, &error))
				goto error;
			goto cleanup;
		}
	}

	/*
	 * If munged file didn't quit, switch into interactive mode
	 */
	/* commandline replacement string register */
	teco_qreg_table_insert(&teco_qreg_table_globals, teco_qreg_plain_new("\e", 1));

	teco_undo_enabled = TRUE;
	teco_ring_set_scintilla_undo(TRUE);
	teco_view_set_scintilla_undo(teco_qreg_view, TRUE);

	/*
	 * FIXME: Perhaps we should simply call teco_cmdline_init() and
	 * teco_cmdline_cleanup() here.
	 */
	teco_machine_main_init(&teco_cmdline.machine, &local_qregs, TRUE);

	if (!teco_interface_event_loop(&error))
		goto error;

	teco_machine_main_clear(&teco_cmdline.machine);
	memset(&teco_cmdline.machine, 0, sizeof(teco_cmdline.machine));

	/*
	 * Ordinary application termination:
	 * Interface is shut down, so we are
	 * in non-interactive mode again.
	 */
	teco_undo_enabled = FALSE;
	teco_undo_clear();
	/* also empties all Scintilla undo buffers */
	teco_ring_set_scintilla_undo(FALSE);
	teco_view_set_scintilla_undo(teco_qreg_view, FALSE);

	if (!teco_ed_hook(TECO_ED_HOOK_QUIT, &error))
		goto error;

cleanup:
#ifndef NDEBUG
	teco_ring_cleanup();
	teco_qreg_table_clear(&local_qregs);
	teco_qreg_table_clear(&teco_qreg_table_globals);
	teco_view_free(teco_qreg_view);
#endif
	teco_interface_cleanup();
	return 0;

error:
	teco_error_display_full(error);
	return EXIT_FAILURE;
}
