#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <locale.h>

#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>

#include <ncurses.h>

#include <Scintilla.h>
#include <ScintillaTerm.h>

#include "sciteco.h"
#include "interface.h"
#include "interface-ncurses.h"

InterfaceNCurses interface;

extern "C" {
static void scnotification(Scintilla *view, int i, void *p1, void *p2);
}

/* FIXME: should be configurable in TECO */
#define ESCAPE_SURROGATE KEY_DC

/* from ScintillaTerm.cxx */
#define SCI_COLOR_PAIR(f, b) ((b) * COLORS + (f) + 1)

InterfaceNCurses::InterfaceNCurses()
		  : popup_window(NULL), popup_list(NULL),
		    popup_list_longest(0), popup_list_length(0)
{
	/*
	 * Prevent the initial redraw and any escape sequences that may
	 * clobber stdout, so we may use the terminal in
	 * cooked mode, for commandline help and batch processing.
	 * Scintilla must be initialized for batch processing to work.
	 * (Frankly I have no idea why this works!)
	 */
	screen_tty = g_fopen("/dev/tty", "r+b");
	screen = newterm(NULL, screen_tty, screen_tty);
	set_term(screen);

	raw();
	cbreak();
	noecho();
	curs_set(0); /* Scintilla draws its own cursor */

	setlocale(LC_CTYPE, ""); /* for displaying UTF-8 characters properly */

	/* NOTE: initializes color pairs */
	sci = scintilla_new(scnotification);
	sci_window = scintilla_get_window(sci);
	wresize(sci_window, LINES - 2, COLS);

	msg_window = newwin(1, 0, LINES - 2, 0);

	cmdline_window = newwin(0, 0, LINES - 1, 0);
	keypad(cmdline_window, TRUE);
	cmdline_current = NULL;

	ssm(SCI_SETFOCUS, TRUE);

	/* scintilla will be refreshed in event loop */
	msg(MSG_USER, " ");
	cmdline_update("");

	endwin();
}

void
InterfaceNCurses::resize_all_windows(void)
{
	int lines, cols;

	getmaxyx(stdscr, lines, cols);

	wresize(sci_window, lines - 2, cols);
	wresize(msg_window, 1, cols);
	mvwin(msg_window, lines - 2, 0);
	wresize(cmdline_window, 1, cols);
	mvwin(cmdline_window, lines - 1, 0);

	/* scintilla will be refreshed in event loop */
	msg(MSG_USER, " "); /* FIXME: use saved message */
	cmdline_update();
}

void
InterfaceNCurses::vmsg(MessageType type, const gchar *fmt, va_list ap)
{
	static const short type2colorid[] = {
		SCI_COLOR_PAIR(COLOR_BLACK, COLOR_WHITE),  /* MSG_USER */
		SCI_COLOR_PAIR(COLOR_BLACK, COLOR_GREEN),  /* MSG_INFO */
		SCI_COLOR_PAIR(COLOR_BLACK, COLOR_YELLOW), /* MSG_WARNING */
		SCI_COLOR_PAIR(COLOR_BLACK, COLOR_RED)	   /* MSG_ERROR */
	};

	if (isendwin()) { /* batch mode */
		stdio_vmsg(type, fmt, ap);
		return;
	}

	wmove(msg_window, 0, 0);
	wbkgdset(msg_window, ' ' | COLOR_PAIR(type2colorid[type]));
	vw_printw(msg_window, fmt, ap);
	wclrtoeol(msg_window);

	wrefresh(msg_window);
}

void
InterfaceNCurses::cmdline_update(const gchar *cmdline)
{
	size_t len;
	int half_line = (getmaxx(stdscr) - 2) / 2;
	const gchar *line;

	if (cmdline) {
		g_free(cmdline_current);
		cmdline_current = g_strdup(cmdline);
	} else {
		cmdline = cmdline_current;
	}
	len = strlen(cmdline);

	/* FIXME: optimize */
	line = cmdline + len - MIN(len, half_line + len % half_line);

	mvwaddch(cmdline_window, 0, 0, '*');
	waddstr(cmdline_window, line);
	waddch(cmdline_window, ' ' | A_REVERSE);
	wclrtoeol(cmdline_window);

	wrefresh(cmdline_window);
}

void
InterfaceNCurses::popup_add_filename(PopupFileType type,
				     const gchar *filename, bool highlight)
{
	gchar *entry = g_strconcat(highlight ? "*" : " ", filename, NULL);

	popup_list_longest = MAX(popup_list_longest, (gint)strlen(filename));
	popup_list_length++;

	popup_list = g_slist_prepend(popup_list, entry);
}

void
InterfaceNCurses::popup_show(void)
{
	int popup_lines;
	gint popup_cols, cur_file;

	popup_list_longest += 3;
	popup_list = g_slist_reverse(popup_list);

	popup_cols = MIN(popup_list_length, COLS / popup_list_longest);
	popup_lines = (popup_list_length + popup_list_length % popup_cols)/
		      popup_cols;
	/* window covers message and scintilla windows */
	popup_window = newwin(popup_lines, 0, LINES - 1 - popup_lines, 0);
	wbkgdset(popup_window,
		 ' ' | COLOR_PAIR(SCI_COLOR_PAIR(COLOR_BLACK, COLOR_BLUE)));

	cur_file = 0;
	for (GSList *cur = popup_list; cur; cur = g_slist_next(cur)) {
		gchar *entry = (gchar *)cur->data;

		if (cur_file && !(cur_file % popup_cols)) {
			wclrtoeol(popup_window);
			waddch(popup_window, '\n');
		}

		(void)wattrset(popup_window, *entry == '*' ? A_BOLD : A_NORMAL);
		waddstr(popup_window, entry + 1);
		for (int i = popup_list_longest - strlen(entry) + 1; i; i--)
			waddch(popup_window, ' ');

		g_free(cur->data);
		cur_file++;
	}
	wclrtoeol(popup_window);

	g_slist_free(popup_list);
	popup_list = NULL;
	popup_list_longest = 0;
	popup_list_length = 0;
}

void
InterfaceNCurses::popup_clear(void)
{
	if (!popup_window)
		return;

	scintilla_refresh(sci);
	redrawwin(msg_window);
	wrefresh(msg_window);
	delwin(popup_window);
	popup_window = NULL;
}

void
InterfaceNCurses::event_loop(void)
{
	/* in commandline (visual) mode, enforce redraw */
	wrefresh(curscr);

	for (;;) {
		int key;

		/* also handles initial refresh (styles are configured...) */
		scintilla_refresh(sci);
		if (popup_window)
			wrefresh(popup_window);

		key = wgetch(cmdline_window);
		switch (key) {
		case ERR:
		case KEY_RESIZE:
			resize_all_windows();
			break;
		case ESCAPE_SURROGATE:
			cmdline_keypress('\x1B');
			break;
		case KEY_BACKSPACE:
			cmdline_keypress('\b');
			break;
		case KEY_ENTER:
			switch (ssm(SCI_GETEOLMODE)) {
			case SC_EOL_CR:
				cmdline_keypress('\r');
				break;
			case SC_EOL_CRLF:
				cmdline_keypress('\r');
				/* fall through */
			case SC_EOL_LF:
			default:
				cmdline_keypress('\n');
			}
			break;
		default:
			if (key <= 0xFF)
				cmdline_keypress((gchar)key);
		}
	}
}

InterfaceNCurses::~InterfaceNCurses()
{
	scintilla_delete(sci);
	delwin(sci_window);

	delwin(cmdline_window);
	g_free(cmdline_current);
	delwin(msg_window);

	if (popup_window)
		delwin(popup_window);
	if (popup_list)
		g_slist_free(popup_list);

	if (!isendwin())
		endwin();
	delscreen(screen);
	fclose(screen_tty);
}

/*
 * Callbacks
 */

static void
scnotification(Scintilla *view, int i, void *p1, void *p2)
{
	/* TODO */
}
