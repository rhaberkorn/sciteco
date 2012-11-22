#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <locale.h>

#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>

#include <curses.h>

#include <Scintilla.h>
#include <ScintillaTerm.h>

#include "sciteco.h"
#include "qbuffers.h"
#include "interface.h"
#include "interface-ncurses.h"

InterfaceNCurses interface;

extern "C" {
static void scintilla_notify(Scintilla *sci, int idFrom,
			     void *notify, void *user_data);
}

#define UNNAMED_FILE "(Unnamed)"

/* FIXME: should be configurable in TECO (Function key substitutes) */
#define ESCAPE_SURROGATE KEY_DC

/* from ScintillaTerm.cxx */
#define SCI_COLOR_PAIR(f, b) \
	((b) * COLORS + (f) + 1)

#define SCI_COLOR_ATTR(f, b) \
	COLOR_PAIR(SCI_COLOR_PAIR(f, b))

InterfaceNCurses::InterfaceNCurses()
{
#ifdef __PDCURSES__
	initscr();
	screen = NULL;
#else
	/*
	 * Prevent the initial redraw and any escape sequences that may
	 * interfere with stdout, so we may use the terminal in
	 * cooked mode, for commandline help and batch processing.
	 * Scintilla must be initialized for batch processing to work.
	 * (Frankly I have no idea why this works!)
	 */
	screen_tty = g_fopen("/dev/tty", "r+b");
	screen = newterm(NULL, screen_tty, screen_tty);
	set_term(screen);
#endif

	raw();
	cbreak();
	noecho();
	curs_set(0); /* Scintilla draws its own cursor */

	setlocale(LC_CTYPE, ""); /* for displaying UTF-8 characters properly */

	info_window = newwin(1, 0, 0, 0);
	info_current = g_strdup(PACKAGE_NAME);

	/* NOTE: Scintilla initializes color pairs */
	sci = scintilla_new(scintilla_notify);
	sci_window = scintilla_get_window(sci);
	wresize(sci_window, LINES - 3, COLS);
	mvwin(sci_window, 1, 0);

	msg_window = newwin(1, 0, LINES - 2, 0);

	cmdline_window = newwin(0, 0, LINES - 1, 0);
	keypad(cmdline_window, TRUE);
	cmdline_current = NULL;

	ssm(SCI_SETFOCUS, TRUE);

	draw_info();
	/* scintilla will be refreshed in event loop */
	msg_clear();
	cmdline_update("");

	endwin();
}

void
InterfaceNCurses::resize_all_windows(void)
{
	int lines, cols; /* screen dimensions */

	getmaxyx(stdscr, lines, cols);

	wresize(info_window, 1, cols);
	wresize(sci_window, lines - 3, cols);
	wresize(msg_window, 1, cols);
	mvwin(msg_window, lines - 2, 0);
	wresize(cmdline_window, 1, cols);
	mvwin(cmdline_window, lines - 1, 0);

	draw_info();
	/* scintilla will be refreshed in event loop */
	msg_clear(); /* FIXME: use saved message */
	cmdline_update();
}

void
InterfaceNCurses::vmsg(MessageType type, const gchar *fmt, va_list ap)
{
	static const int type2attr[] = {
		SCI_COLOR_ATTR(COLOR_BLACK, COLOR_WHITE),  /* MSG_USER */
		SCI_COLOR_ATTR(COLOR_BLACK, COLOR_GREEN),  /* MSG_INFO */
		SCI_COLOR_ATTR(COLOR_BLACK, COLOR_YELLOW), /* MSG_WARNING */
		SCI_COLOR_ATTR(COLOR_BLACK, COLOR_RED)	   /* MSG_ERROR */
	};

	if (isendwin()) { /* batch mode */
		stdio_vmsg(type, fmt, ap);
		return;
	}

	wmove(msg_window, 0, 0);
	wbkgdset(msg_window, ' ' | type2attr[type]);
	vw_printw(msg_window, fmt, ap);
	wclrtoeol(msg_window);

	wrefresh(msg_window);
}

void
InterfaceNCurses::msg_clear(void)
{
	if (isendwin()) /* batch mode */
		return;

	wmove(msg_window, 0, 0);
	wbkgdset(msg_window, ' ' | SCI_COLOR_ATTR(COLOR_BLACK, COLOR_WHITE));
	wclrtoeol(msg_window);

	wrefresh(msg_window);
}

void
InterfaceNCurses::draw_info(void)
{
	if (isendwin()) /* batch mode */
		return;

	wmove(info_window, 0, 0);
	wbkgdset(info_window, ' ' | SCI_COLOR_ATTR(COLOR_BLACK, COLOR_WHITE));
	waddstr(info_window, info_current);
	wclrtoeol(info_window);

	wrefresh(info_window);
}

void
InterfaceNCurses::info_update(QRegister *reg)
{
	g_free(info_current);
	info_current = g_strdup_printf("%s - <QRegister> %s", PACKAGE_NAME,
				       reg->name);

	draw_info();
}

void
InterfaceNCurses::info_update(Buffer *buffer)
{
	g_free(info_current);
	info_current = g_strdup_printf("%s - <Buffer> %s%s", PACKAGE_NAME,
				       buffer->filename ? : UNNAMED_FILE,
				       buffer->dirty ? "*" : "");

	draw_info();
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
	gchar *entry;

	if (isendwin()) /* batch mode */
		return;

	entry = g_strconcat(highlight ? "*" : " ", filename, NULL);

	popup.longest = MAX(popup.longest, (gint)strlen(filename));
	popup.length++;

	popup.list = g_slist_prepend(popup.list, entry);
}

void
InterfaceNCurses::popup_show(void)
{
	int lines, cols; /* screen dimensions */
	int popup_lines;
	gint popup_cols, cur_file;

	if (isendwin()) /* batch mode */
		goto cleanup;

	getmaxyx(stdscr, lines, cols);

	popup.longest += 3;
	popup.list = g_slist_reverse(popup.list);

	popup_cols = MIN(popup.length, cols / popup.longest);
	popup_lines = (popup.length + popup.length % popup_cols)/popup_cols;
	/* window covers message and scintilla windows */
	popup.window = newwin(popup_lines, 0, lines - 1 - popup_lines, 0);
	wbkgdset(popup.window, ' ' | SCI_COLOR_ATTR(COLOR_BLACK, COLOR_BLUE));

	cur_file = 0;
	for (GSList *cur = popup.list; cur; cur = g_slist_next(cur)) {
		gchar *entry = (gchar *)cur->data;

		if (cur_file && !(cur_file % popup_cols)) {
			wclrtoeol(popup.window);
			waddch(popup.window, '\n');
		}

		(void)wattrset(popup.window, *entry == '*' ? A_BOLD : A_NORMAL);
		waddstr(popup.window, entry + 1);
		for (int i = popup.longest - strlen(entry) + 1; i; i--)
			waddch(popup.window, ' ');

		g_free(cur->data);
		cur_file++;
	}
	wclrtoeol(popup.window);

cleanup:
	g_slist_free(popup.list);
	popup.list = NULL;
	popup.longest = popup.length = 0;
}

void
InterfaceNCurses::popup_clear(void)
{
	if (!popup.window)
		return;

	redrawwin(info_window);
	wrefresh(info_window);
	redrawwin(sci_window);
	scintilla_refresh(sci);
	redrawwin(msg_window);
	wrefresh(msg_window);

	delwin(popup.window);
	popup.window = NULL;
}

void
InterfaceNCurses::event_loop(void)
{
	/* in commandline (visual) mode, enforce redraw */
	wrefresh(curscr);
	draw_info();

	for (;;) {
		int key;

		/* also handles initial refresh (styles are configured...) */
		scintilla_refresh(sci);
		if (popup.window)
			wrefresh(popup.window);

		key = wgetch(cmdline_window);
		switch (key) {
#ifdef KEY_RESIZE
		case ERR:
		case KEY_RESIZE:
#ifdef PDCURSES
			resize_term(0, 0);
#endif
			resize_all_windows();
			break;
#endif
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

InterfaceNCurses::Popup::~Popup()
{
	if (window)
		delwin(window);
	if (list)
		g_slist_free(list);
}

InterfaceNCurses::~InterfaceNCurses()
{
	delwin(info_window);
	g_free(info_current);
	/* also deletes curses window */
	scintilla_delete(sci);
	delwin(cmdline_window);
	g_free(cmdline_current);
	delwin(msg_window);

	if (!isendwin())
		endwin();
#ifndef __PDCURSES__
	delscreen(screen);
	fclose(screen_tty);
#endif
}

/*
 * Callbacks
 */

static void
scintilla_notify(Scintilla *sci, int idFrom, void *notify, void *user_data)
{
	interface.process_notify((SCNotification *)notify);
}
