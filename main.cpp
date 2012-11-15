#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>

#include <Scintilla.h>
#include <SciLexer.h>

#include "sciteco.h"
#include "interface.h"
#include "parser.h"
#include "qbuffers.h"
#include "undo.h"

#define INI_FILE ".teco_ini"

static gchar *mung_file = NULL;

static inline void
process_options(int &argc, char **&argv)
{
	static const GOptionEntry option_entries[] = {
		{"mung", 'm', 0, G_OPTION_ARG_FILENAME, &mung_file,
		 "Mung file instead of " INI_FILE, "filename"},
		{NULL}
	};

	GOptionContext *options;

	options = g_option_context_new("- Advanced interactive TECO");
	g_option_context_add_main_entries(options, option_entries, NULL);
	g_option_context_add_group(options, gtk_get_option_group(TRUE));
	if (!g_option_context_parse(options, &argc, &argv, NULL)) {
		g_printf("Option parsing failed!\n");
		exit(EXIT_FAILURE);
	}
	g_option_context_free(options);

	if (mung_file) {
		if (!g_file_test(mung_file, G_FILE_TEST_IS_REGULAR)) {
			g_printf("Cannot mung %s. File does not exist!\n",
				 mung_file);
			exit(EXIT_FAILURE);
		}
	} else {
		mung_file = g_build_filename(g_get_user_config_dir(),
					     INI_FILE, NULL);
	}

	interface.parse_args(argc, argv);

	/* remaining arguments, are arguments to the munged file */
}

int
main(int argc, char **argv)
{
	process_options(argc, argv);

	interface.ssm(SCI_SETFOCUS, 1);
	interface.ssm(SCI_SETCARETSTYLE, 2);
	interface.ssm(SCI_STYLESETFONT, STYLE_DEFAULT, (sptr_t)"Courier");
	interface.ssm(SCI_STYLECLEARALL);
	interface.ssm(SCI_SETLEXER, SCLEX_CPP);
	interface.ssm(SCI_SETKEYWORDS, 0, (sptr_t)"int char");
	interface.ssm(SCI_STYLESETFORE, SCE_C_COMMENT, 0x008000);
	interface.ssm(SCI_STYLESETFORE, SCE_C_COMMENTLINE, 0x008000);
	interface.ssm(SCI_STYLESETFORE, SCE_C_NUMBER, 0x808000);
	interface.ssm(SCI_STYLESETFORE, SCE_C_WORD, 0x800000);
	interface.ssm(SCI_STYLESETFORE, SCE_C_STRING, 0x800080);
	interface.ssm(SCI_STYLESETBOLD, SCE_C_OPERATOR, 1);

	qregisters.initialize();
	ring.edit(NULL);

	/* add remaining arguments to unnamed buffer */
	for (int i = 1; i < argc; i++) {
		interface.ssm(SCI_ADDTEXT, strlen(argv[i]), (sptr_t)argv[i]);
		interface.ssm(SCI_ADDTEXT, 1, (sptr_t)"\r");
	}
	interface.ssm(SCI_GOTOPOS, 0);

	if (g_file_test(mung_file, G_FILE_TEST_IS_REGULAR)) {
		if (!file_execute(mung_file))
			exit(EXIT_FAILURE);
		/* FIXME: make quit immediate in commandline mode (non-UNDO)? */
		if (quit_requested) {
			/* FIXME */
			exit(EXIT_SUCCESS);
		}
	}
	g_free(mung_file);

	undo.enabled = true;

	interface.cmdline_update("*");

	interface.event_loop();

	return 0;
}
