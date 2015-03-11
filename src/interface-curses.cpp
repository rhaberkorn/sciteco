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

#ifdef EMSCRIPTEN
#include <emscripten.h>
#endif

#include "sciteco.h"
#include "string-utils.h"
#include "cmdline.h"
#include "qregisters.h"
#include "ring.h"
#include "interface.h"
#include "interface-curses.h"

/**
 * Whether we have PDCurses-only routines:
 * Could be 0, even on PDCurses
 */
#ifndef PDCURSES
#define PDCURSES 0
#endif

/**
 * Whether we're on PDCurses/win32
 */
#if defined(__PDCURSES__) && defined(G_OS_WIN32) && \
    !defined(PDCURSES_WIN32A)
#define PDCURSES_WIN32
#endif

namespace SciTECO {

extern "C" {
static void scintilla_notify(Scintilla *sci, int idFrom,
			    void *notify, void *user_data);
}

#define UNNAMED_FILE "(Unnamed)"

#define SCI_COLOR_ATTR(f, b) \
	((attr_t)COLOR_PAIR(SCI_COLOR_PAIR(f, b)))

void
ViewCurses::initialize_impl(void)
{
	WINDOW *window;

	/* NOTE: Scintilla initializes color pairs */
	sci = scintilla_new(scintilla_notify);
	window = get_window();

	/*
	 * Window must have dimension before it can be
	 * positioned.
	 * Perhaps it's better to leave the window
	 * unitialized and set the position in
	 * InterfaceCurses::show_view().
	 */
	wresize(window, 1, 1);
	/* Set up window position: never changes */
	mvwin(window, 1, 0);

	setup();
}

void
InterfaceCurses::main_impl(int &argc, char **&argv)
{
	init_screen();
	cbreak();
	noecho();
	curs_set(0); /* Scintilla draws its own cursor */

	setlocale(LC_CTYPE, ""); /* for displaying UTF-8 characters properly */

	info_window = newwin(1, 0, 0, 0);
	info_current = g_strdup(PACKAGE_NAME);

	msg_window = newwin(1, 0, LINES - 2, 0);

	cmdline_window = newwin(0, 0, LINES - 1, 0);

#ifdef EMSCRIPTEN
        nodelay(cmdline_window, TRUE);
#else
#ifndef PDCURSES_WIN32A
	/* workaround: endwin() is somewhat broken in the win32a port */
	endwin();
#endif
#endif
}

#ifdef __PDCURSES__ /* Any PDCurses */

void
InterfaceCurses::init_screen(void)
{
#ifdef PDCURSES_WIN32A
	/* enables window resizing on Win32a port */
	PDC_set_resize_limits(25, 0xFFFF, 80, 0xFFFF);
#endif

	initscr();

	screen_tty = NULL;
	screen = NULL;
}

#else

void
InterfaceCurses::init_screen(void)
{
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
}

#endif /* !__PDCURSES__ */

void
InterfaceCurses::resize_all_windows(void)
{
	int lines, cols; /* screen dimensions */

	getmaxyx(stdscr, lines, cols);

	wresize(info_window, 1, cols);
	wresize(current_view->get_window(),
	        lines - 3, cols);
	wresize(msg_window, 1, cols);
	mvwin(msg_window, lines - 2, 0);
	wresize(cmdline_window, 1, cols);
	mvwin(cmdline_window, lines - 1, 0);

	draw_info();
	msg_clear(); /* FIXME: use saved message */
	popup_clear();
	draw_cmdline();
}

void
InterfaceCurses::vmsg_impl(MessageType type, const gchar *fmt, va_list ap)
{
	static const attr_t type2attr[] = {
		SCI_COLOR_ATTR(COLOR_BLACK, COLOR_WHITE),  /* MSG_USER */
		SCI_COLOR_ATTR(COLOR_BLACK, COLOR_GREEN),  /* MSG_INFO */
		SCI_COLOR_ATTR(COLOR_BLACK, COLOR_YELLOW), /* MSG_WARNING */
		SCI_COLOR_ATTR(COLOR_BLACK, COLOR_RED)	   /* MSG_ERROR */
	};

#ifdef PDCURSES_WIN32A
	stdio_vmsg(type, fmt, ap);
	if (isendwin()) /* batch mode */
		return;
#else
	if (isendwin()) { /* batch mode */
		stdio_vmsg(type, fmt, ap);
		return;
	}
#endif

	wmove(msg_window, 0, 0);
	wbkgdset(msg_window, ' ' | type2attr[type]);
	vw_printw(msg_window, fmt, ap);
	wclrtoeol(msg_window);
}

void
InterfaceCurses::msg_clear(void)
{
	if (isendwin()) /* batch mode */
		return;

	wmove(msg_window, 0, 0);
	wbkgdset(msg_window, ' ' | SCI_COLOR_ATTR(COLOR_BLACK, COLOR_WHITE));
	wclrtoeol(msg_window);
}

void
InterfaceCurses::show_view_impl(ViewCurses *view)
{
	int lines, cols; /* screen dimensions */

	current_view = view;

	/*
	 * screen size might have changed since
	 * this view's WINDOW was last active
	 */
	getmaxyx(stdscr, lines, cols);
	wresize(current_view->get_window(),
	        lines - 3, cols);
}

void
InterfaceCurses::draw_info(void)
{
	if (isendwin()) /* batch mode */
		return;

	wmove(info_window, 0, 0);
	wbkgdset(info_window, ' ' | SCI_COLOR_ATTR(COLOR_BLACK, COLOR_WHITE));
	waddstr(info_window, info_current);
	wclrtoeol(info_window);

#if PDCURSES
	PDC_set_title(info_current);
#endif
}

void
InterfaceCurses::info_update_impl(const QRegister *reg)
{
	/*
	 * We cannot rely on Curses' control character drawing
	 * and we need the info_current string for other purposes
	 * (like PDC_set_title()), so we "canonicalize" the
	 * register name here:
	 */
	gchar *name = String::canonicalize_ctl(reg->name);

	g_free(info_current);
	info_current = g_strconcat(PACKAGE_NAME " - <QRegister> ",
	                           name, NIL);
	g_free(name);
	/* NOTE: drawn in event_loop_iter() */
}

void
InterfaceCurses::info_update_impl(const Buffer *buffer)
{
	g_free(info_current);
	info_current = g_strconcat(PACKAGE_NAME " - <Buffer> ",
	                           buffer->filename ? : UNNAMED_FILE,
	                           buffer->dirty ? "*" : "", NIL);
	/* NOTE: drawn in event_loop_iter() */
}

void
InterfaceCurses::format_chr(chtype *&target, gchar chr, attr_t attr)
{
	/*
	 * NOTE: This mapping is similar to
	 * View::set_representations()
	 */
	switch (chr) {
	case CTL_KEY_ESC:
		*target++ = '$' | attr | A_REVERSE;
		break;
	case '\r':
		*target++ = 'C' | attr | A_REVERSE;
		*target++ = 'R' | attr | A_REVERSE;
		break;
	case '\n':
		*target++ = 'L' | attr | A_REVERSE;
		*target++ = 'F' | attr | A_REVERSE;
		break;
	case '\t':
		*target++ = 'T' | attr | A_REVERSE;
		*target++ = 'A' | attr | A_REVERSE;
		*target++ = 'B' | attr | A_REVERSE;
		break;
	default:
		if (IS_CTL(chr)) {
			*target++ = '^' | attr | A_REVERSE;
			*target++ = CTL_ECHO(chr) | attr | A_REVERSE;
		} else {
			*target++ = chr | attr;
		}
	}
}

void
InterfaceCurses::cmdline_update_impl(const Cmdline *cmdline)
{
	gsize alloc_len = 1;
	chtype *p;

	/*
	 * AFAIK bold black should be rendered grey by any
	 * common terminal.
	 * If not, this problem will be gone once we support
	 * a Scintilla view command line.
	 * Also A_UNDERLINE is not supported by PDCurses/win32
	 * and causes weird colors, so we better leave it away.
	 */
	static const attr_t rubout_attr =
#ifndef PDCURSES_WIN32
		A_UNDERLINE |
#endif
		A_BOLD | SCI_COLOR_ATTR(COLOR_BLACK, COLOR_BLACK);

	/*
	 * Replace entire pre-formatted command-line.
	 * We don't know if it is similar to the last one,
	 * so realloc makes no sense.
	 * We approximate the size of the new formatted command-line,
	 * wasting a few bytes for control characters.
	 */
	delete[] cmdline_current;
	for (guint i = 0; i < cmdline->len+cmdline->rubout_len; i++)
		alloc_len += IS_CTL((*cmdline)[i]) ? 3 : 1;
	p = cmdline_current = new chtype[alloc_len];

	/* format effective command line */
	for (guint i = 0; i < cmdline->len; i++)
		format_chr(p, (*cmdline)[i]);
	cmdline_len = p - cmdline_current;

	/* Format rubbed-out command line. */
	for (guint i = cmdline->len; i < cmdline->len+cmdline->rubout_len; i++)
		format_chr(p, (*cmdline)[i], rubout_attr);
	cmdline_rubout_len = p - cmdline_current - cmdline_len;

	/* highlight cursor after effective command line */
	if (cmdline_rubout_len) {
		cmdline_current[cmdline_len] &= A_CHARTEXT | A_UNDERLINE;
		cmdline_current[cmdline_len] |= A_REVERSE;
	} else {
		cmdline_current[cmdline_len++] = ' ' | A_REVERSE;
	}

	draw_cmdline();
}

void
InterfaceCurses::draw_cmdline(void)
{
	/* total width available for command line */
	guint total_width = getmaxx(stdscr) - 1;
	/* beginning of command line to show */
	guint disp_offset;
	/* length of command line to show */
	guint disp_len;

	disp_offset = cmdline_len -
	              MIN(cmdline_len, total_width/2 + cmdline_len % (total_width/2));
	disp_len = MIN(total_width, cmdline_len+cmdline_rubout_len - disp_offset);

	werase(cmdline_window);
	mvwaddch(cmdline_window, 0, 0, '*' | A_BOLD);
	waddchnstr(cmdline_window, cmdline_current+disp_offset, disp_len);
}

void
InterfaceCurses::popup_add_impl(PopupEntryType type,
			         const gchar *name, bool highlight)
{
	gchar *entry;

	if (isendwin()) /* batch mode */
		return;

	entry = g_strconcat(highlight ? "*" : " ", name, NIL);

	popup.longest = MAX(popup.longest, (gint)strlen(name));
	popup.length++;

	popup.list = g_slist_prepend(popup.list, entry);
}

void
InterfaceCurses::popup_show_impl(void)
{
	int lines, cols; /* screen dimensions */
	int popup_lines;
	gint popup_cols;
	gint popup_colwidth;
	gint cur_col;

	if (isendwin() || !popup.length)
		/* batch mode or nothing to display */
		return;

	getmaxyx(stdscr, lines, cols);

	if (popup.window)
		delwin(popup.window);
	else
		/* reverse list only once */
		popup.list = g_slist_reverse(popup.list);

	if (!popup.cur_list) {
		/* start from beginning of list */
		popup.cur_list = popup.list;
		popup.cur_entry = 0;
	}

	/* reserve 2 spaces between columns */
	popup_colwidth = popup.longest + 2;

	/* popup_cols = floor(cols / popup_colwidth) */
	popup_cols = MAX(cols / popup_colwidth, 1);
	/* popup_lines = ceil((popup.length-popup.cur_entry) / popup_cols) */
	popup_lines = (popup.length-popup.cur_entry+popup_cols-1) / popup_cols;
	/*
	 * Popup window can cover all but one screen row.
	 * If it does not fit, the list of tokens is truncated
	 * and "..." is displayed.
	 */
	popup_lines = MIN(popup_lines, lines - 1);

	/* window covers message, scintilla and info windows */
	popup.window = newwin(popup_lines, 0, lines - 1 - popup_lines, 0);
	wbkgd(popup.window, ' ' | SCI_COLOR_ATTR(COLOR_BLACK, COLOR_BLUE));

	/*
	 * cur_col is the row currently written.
	 * It does not wrap but grows indefinitely.
	 * Therefore the real current row is (cur_col % popup_cols)
	 */
	cur_col = 0;
	while (popup.cur_list) {
		gchar *entry = (gchar *)popup.cur_list->data;
		gint cur_line = cur_col/popup_cols + 1;

		wmove(popup.window,
		      cur_line-1, (cur_col % popup_cols)*popup_colwidth);
		cur_col++;

		if (cur_line == popup_lines && !(cur_col % popup_cols) &&
		    g_slist_next(popup.cur_list) != NULL) {
			/* truncate entries in the popup's very last column */
			(void)wattrset(popup.window, A_BOLD);
			waddstr(popup.window, "...");
			break;
		}

		(void)wattrset(popup.window, *entry == '*' ? A_BOLD : A_NORMAL);
		waddstr(popup.window, entry + 1);

		popup.cur_list = g_slist_next(popup.cur_list);
		popup.cur_entry++;
	}

	redrawwin(info_window);
	/* scintilla window is redrawn by ViewCurses::refresh() */
	redrawwin(msg_window);
}

void
InterfaceCurses::popup_clear_impl(void)
{
	g_slist_free_full(popup.list, g_free);
	popup.list = NULL;
	popup.length = 0;
	/* reserve at least 3 characters for "..." */
	popup.longest = 3;

	popup.cur_list = NULL;
	popup.cur_entry = 0;

	if (!popup.window)
		return;

	redrawwin(info_window);
	/* scintilla window is redrawn by ViewCurses::refresh() */
	redrawwin(msg_window);

	delwin(popup.window);
	popup.window = NULL;
}

/**
 * One iteration of the event loop.
 *
 * This is a global function, so it may
 * be used as an Emscripten callback.
 *
 * @bug
 * Can probably be defined as a static method,
 * so we can avoid declaring it a fried function of
 * InterfaceCurses.
 */
void
event_loop_iter()
{
	int key;

	/*
	 * Setting function key processing is important
	 * on Unix Curses, as ESCAPE is handled as the beginning
	 * of a escape sequence when terminal emulators are
	 * involved.
	 */
	keypad(interface.cmdline_window, Flags::ed & Flags::ED_FNKEYS);

	/* no special <CTRL/C> handling */
	raw();
	key = wgetch(interface.cmdline_window);
	/* allow asynchronous interruptions on <CTRL/C> */
	cbreak();
	if (key == ERR)
		return;

	switch (key) {
#ifdef KEY_RESIZE
	case KEY_RESIZE:
#ifdef __PDCURSES__
		resize_term(0, 0);
#endif
		interface.resize_all_windows();
		break;
#endif
	case CTL_KEY('H'):
	case 0x7F: /* ^? */
	case KEY_BACKSPACE:
		/*
		 * For historic reasons terminals can send
		 * ASCII 8 (^H) or 127 (^?) for backspace.
		 * Curses also defines KEY_BACKSPACE, probably
		 * for terminals that send an escape sequence for
		 * backspace.
		 * In SciTECO backspace is normalized to ^H.
		 */
		cmdline.keypress(CTL_KEY('H'));
		break;
	case KEY_ENTER:
	case '\r':
	case '\n':
		cmdline.keypress(get_eol());
		break;

	/*
	 * Function key macros
	 */
#define FN(KEY) case KEY_##KEY: cmdline.fnmacro(#KEY); break
#define FNS(KEY) FN(KEY); FN(S##KEY)
	FN(DOWN); FN(UP); FNS(LEFT); FNS(RIGHT);
	FNS(HOME);
	case KEY_F(0)...KEY_F(63): {
		gchar macro_name[3+1];

		g_snprintf(macro_name, sizeof(macro_name),
			   "F%d", key - KEY_F0);
		cmdline.fnmacro(macro_name);
		break;
	}
	FNS(DC);
	FNS(IC);
	FN(NPAGE); FN(PPAGE);
	FNS(PRINT);
	FN(A1); FN(A3); FN(B2); FN(C1); FN(C3);
	FNS(END);
	FNS(HELP);
#undef FNS
#undef FN

	/*
	 * Control keys and keys with printable representation
	 */
	default:
		if (key <= 0xFF)
			cmdline.keypress((gchar)key);
	}

	sigint_occurred = FALSE;

	/*
	 * Info window is updated very often which is very
	 * costly, especially when using PDC_set_title(),
	 * so we redraw it here, where the overhead does
	 * not matter much.
	 */
	interface.draw_info();
	wnoutrefresh(interface.info_window);
	/* FIXME: this does wrefresh() internally */
	interface.current_view->refresh();
	wnoutrefresh(interface.msg_window);
	wnoutrefresh(interface.cmdline_window);
	if (interface.popup.window)
		wnoutrefresh(interface.popup.window);
	doupdate();
}

void
InterfaceCurses::event_loop_impl(void)
{
	static const Cmdline empty_cmdline;

	/* initial refresh */
	/* FIXME: this does wrefresh() internally */
	current_view->refresh();
	draw_info();
	wnoutrefresh(info_window);
	msg_clear();
	wnoutrefresh(msg_window);
	cmdline_update(&empty_cmdline);
	wnoutrefresh(cmdline_window);
	doupdate();

#ifdef EMSCRIPTEN
	PDC_emscripten_set_handler(event_loop_iter, TRUE);
	/*
	 * We must not block emscripten's main loop,
	 * instead event_loop_iter() is called asynchronously.
	 * We also must not exit the event_loop() method, since
	 * SciTECO would assume ordinary program termination.
	 * We also must not call exit() since that would run
	 * the global destructors.
	 * The following exits the main() function immediately
	 * while keeping the "runtime" alive.
	 */
	emscripten_exit_with_live_runtime();
#else
	try {
		for (;;)
			event_loop_iter();
	} catch (Quit) {
		/* SciTECO termination (e.g. EX$$) */
	}

	/*
	 * Restore ordinary terminal behaviour
	 */
	endwin();
#endif
}

InterfaceCurses::Popup::~Popup()
{
	if (window)
		delwin(window);
	if (list)
		g_slist_free_full(list, g_free);
}

InterfaceCurses::~InterfaceCurses()
{
	if (info_window)
		delwin(info_window);
	g_free(info_current);
	if (cmdline_window)
		delwin(cmdline_window);
	delete[] cmdline_current;
	if (msg_window)
		delwin(msg_window);

	/* PDCurses (win32) crashes if initscr() wasn't called */
	if (info_window && !isendwin())
		endwin();

	if (screen)
		delscreen(screen);
	if (screen_tty)
		fclose(screen_tty);
}

/*
 * Callbacks
 */

static void
scintilla_notify(Scintilla *sci, int idFrom, void *notify, void *user_data)
{
	interface.process_notify((SCNotification *)notify);
}

} /* namespace SciTECO */
