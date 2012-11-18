#include <stdarg.h>
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
#include "goto.h"
#include "qbuffers.h"
#include "undo.h"

#define INI_FILE ".teco_ini"

static gchar *mung_file = NULL;

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

	interface.ssm(SCI_SETCARETSTYLE, CARETSTYLE_BLOCK);
	interface.ssm(SCI_SETCARETFORE, 0xFFFFFF);

	/*
	 * FIXME: Styles should probably be set interface-based
	 * (system defaults) and be changeable by TECO macros
	 */
	interface.ssm(SCI_STYLESETFORE, STYLE_DEFAULT, 0xFFFFFF);
	interface.ssm(SCI_STYLESETBACK, STYLE_DEFAULT, 0x000000);
	interface.ssm(SCI_STYLESETFONT, STYLE_DEFAULT, (sptr_t)"Courier");
	interface.ssm(SCI_STYLECLEARALL);
	interface.ssm(SCI_STYLESETFORE, SCE_C_COMMENT, 0x00FF00);
	interface.ssm(SCI_STYLESETFORE, SCE_C_COMMENTLINE, 0x00FF00);
	interface.ssm(SCI_STYLESETFORE, SCE_C_NUMBER, 0xFFFF00);
	interface.ssm(SCI_STYLESETFORE, SCE_C_WORD, 0xFF0000);
	interface.ssm(SCI_STYLESETFORE, SCE_C_STRING, 0xFF00FF);
	interface.ssm(SCI_STYLESETBOLD, SCE_C_OPERATOR, TRUE);

	qregisters.initialize();
	ring.edit(NULL);

	/* add remaining arguments to unnamed buffer */
	for (int i = 1; i < argc; i++) {
		interface.ssm(SCI_APPENDTEXT, strlen(argv[i]), (sptr_t)argv[i]);
		interface.ssm(SCI_APPENDTEXT, 1, (sptr_t)"\n");
	}

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

	interface.ssm(SCI_EMPTYUNDOBUFFER);
	goto_table_clear();
	undo.enabled = true;

	interface.event_loop();

	return 0;
}
