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
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <locale.h>
#include <errno.h>

#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>

#include <curses.h>

#ifdef HAVE_TIGETSTR
#include <term.h>

/*
 * Some macros in term.h interfere with our code.
 */
#undef lines
#endif

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

#ifdef HAVE_WINDOWS_H
/* here it shouldn't cause conflicts with other headers */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

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

/*
 * A_UNDERLINE is not supported by PDCurses/win32
 * and causes weird colors, so we simply disable it globally.
 */
#undef  A_UNDERLINE
#define A_UNDERLINE 0
#endif

#ifdef NCURSES_VERSION
#if defined(G_OS_UNIX) || defined(G_OS_HAIKU)
/**
 * Whether we're on ncurses/UNIX.
 * Haiku has a UNIX-like terminal and is largely
 * POSIX compliant, so we can handle it like a
 * UNIX ncurses.
 */
#define NCURSES_UNIX
#elif defined(G_OS_WIN32)
/**
 * Whether we're on ncurses/win32 console
 */
#define NCURSES_WIN32
#endif
#endif

namespace SciTECO {

extern "C" {

/*
 * PDCurses/win32a by default assigns functions to certain
 * keys like CTRL+V, CTRL++, CTRL+- and CTRL+=.
 * This conflicts with SciTECO that must remain in control
 * of keyboard processing.
 * Unfortunately, the default mapping can only be disabled
 * or changed via the internal PDC_set_function_key() in
 * pdcwin.h. Therefore we declare it manually here.
 */
#ifdef PDCURSES_WIN32A
int PDC_set_function_key(const unsigned function, const int new_key);

#define N_FUNCTION_KEYS			5
#define FUNCTION_KEY_SHUT_DOWN		0
#define FUNCTION_KEY_PASTE		1
#define FUNCTION_KEY_ENLARGE_FONT	2
#define FUNCTION_KEY_SHRINK_FONT	3
#define FUNCTION_KEY_CHOOSE_FONT	4
#endif

static void scintilla_notify(Scintilla *sci, int idFrom,
                             void *notify, void *user_data);

#if defined(PDCURSES_WIN32) || defined(NCURSES_WIN32)

/**
 * This handler is the Windows-analogue of a signal
 * handler. MinGW provides signal(), but it's not
 * reliable.
 * This may also be used to handle CTRL_CLOSE_EVENTs.
 * NOTE: Unlike signal handlers, this is executed in a
 * separate thread.
 */
static BOOL WINAPI
console_ctrl_handler(DWORD type)
{
	switch (type) {
	case CTRL_C_EVENT:
		sigint_occurred = TRUE;
		return TRUE;
	}

	return FALSE;
}

#endif

} /* extern "C" */

#define UNNAMED_FILE "(Unnamed)"

/**
 * Get bright variant of one of the 8 standard
 * curses colors.
 * On 8 color terminals, this returns the non-bright
 * color - but you __may__ get a bright version using
 * the A_BOLD attribute.
 * NOTE: This references `COLORS` and is thus not a
 * constant expression.
 */
#define COLOR_LIGHT(C) \
	(COLORS < 16 ? (C) : (C) + 8)

/*
 * The 8 bright colors (if terminal supports at
 * least 16 colors), else they are identical to
 * the non-bright colors (default curses colors).
 */
#define COLOR_LBLACK	COLOR_LIGHT(COLOR_BLACK)
#define COLOR_LRED	COLOR_LIGHT(COLOR_RED)
#define COLOR_LGREEN	COLOR_LIGHT(COLOR_GREEN)
#define COLOR_LYELLOW	COLOR_LIGHT(COLOR_YELLOW)
#define COLOR_LBLUE	COLOR_LIGHT(COLOR_BLUE)
#define COLOR_LMAGENTA	COLOR_LIGHT(COLOR_MAGENTA)
#define COLOR_LCYAN	COLOR_LIGHT(COLOR_CYAN)
#define COLOR_LWHITE	COLOR_LIGHT(COLOR_WHITE)

/**
 * Curses attribute for the color combination
 * `f` (foreground) and `b` (background)
 * according to the color pairs initialized by
 * Scinterm.
 * NOTE: This depends on the global variable
 * `COLORS` and is thus not a constant expression.
 */
#define SCI_COLOR_ATTR(f, b) \
	((attr_t)COLOR_PAIR(SCI_COLOR_PAIR(f, b)))

/**
 * Translate a Scintilla-compatible RGB color value
 * (0xBBGGRR) to a Curses color triple (0 to 1000
 * for each component).
 */
static inline void
rgb2curses(guint32 rgb, short &r, short &g, short &b)
{
	/* NOTE: We could also use 200/51 */
	r = ((rgb & 0x0000FF) >> 0)*1000/0xFF;
	g = ((rgb & 0x00FF00) >> 8)*1000/0xFF;
	b = ((rgb & 0xFF0000) >> 16)*1000/0xFF;
}

/**
 * Convert a Scintilla-compatible RGB color value
 * (0xBBGGRR) to a Curses color code (e.g. COLOR_BLACK).
 * This does not work with arbitrary RGB values but
 * only the 16 RGB color values defined by Scinterm
 * corresponding to the 16 terminal colors.
 * It is equivalent to Scinterm's internal `term_color`
 * function.
 */
static short
rgb2curses(guint32 rgb)
{
	switch (rgb) {
	case 0x000000: return COLOR_BLACK;
	case 0x000080: return COLOR_RED;
	case 0x008000: return COLOR_GREEN;
	case 0x008080: return COLOR_YELLOW;
	case 0x800000: return COLOR_BLUE;
	case 0x800080: return COLOR_MAGENTA;
	case 0x808000: return COLOR_CYAN;
	case 0xC0C0C0: return COLOR_WHITE;
	case 0x404040: return COLOR_LBLACK;
	case 0x0000FF: return COLOR_LRED;
	case 0x00FF00: return COLOR_LGREEN;
	case 0x00FFFF: return COLOR_LYELLOW;
	case 0xFF0000: return COLOR_LBLUE;
	case 0xFF00FF: return COLOR_LMAGENTA;
	case 0xFFFF00: return COLOR_LCYAN;
	case 0xFFFFFF: return COLOR_LWHITE;
	}

	return COLOR_WHITE;
}

static gsize
format_str(WINDOW *win, const gchar *str,
           gssize len = -1, gint max_width = -1)
{
	int old_x, old_y;
	gint chars_added = 0;

	getyx(win, old_y, old_x);

	if (len < 0)
		len = strlen(str);
	if (max_width < 0)
		max_width = getmaxx(win) - old_x;

	while (len > 0) {
		/*
		 * NOTE: This mapping is similar to
		 * View::set_representations()
		 */
		switch (*str) {
		case CTL_KEY_ESC:
			chars_added++;
			if (chars_added > max_width)
				goto truncate;
			waddch(win, '$' | A_REVERSE);
			break;
		case '\r':
			chars_added += 2;
			if (chars_added > max_width)
				goto truncate;
			waddch(win, 'C' | A_REVERSE);
			waddch(win, 'R' | A_REVERSE);
			break;
		case '\n':
			chars_added += 2;
			if (chars_added > max_width)
				goto truncate;
			waddch(win, 'L' | A_REVERSE);
			waddch(win, 'F' | A_REVERSE);
			break;
		case '\t':
			chars_added += 3;
			if (chars_added > max_width)
				goto truncate;
			waddch(win, 'T' | A_REVERSE);
			waddch(win, 'A' | A_REVERSE);
			waddch(win, 'B' | A_REVERSE);
			break;
		default:
			if (IS_CTL(*str)) {
				chars_added += 2;
				if (chars_added > max_width)
					goto truncate;
				waddch(win, '^' | A_REVERSE);
				waddch(win, CTL_ECHO(*str) | A_REVERSE);
			} else {
				chars_added++;
				if (chars_added > max_width)
					goto truncate;
				waddch(win, *str);
			}
		}

		str++;
		len--;
	}

	return getcurx(win) - old_x;

truncate:
	if (max_width >= 3) {
		/*
		 * Truncate string
		 */
		wattron(win, A_UNDERLINE | A_BOLD);
		mvwaddstr(win, old_y, old_x + max_width - 3, "...");
		wattroff(win, A_UNDERLINE | A_BOLD);
	}

	return getcurx(win) - old_x;
}

static gsize
format_filename(WINDOW *win, const gchar *filename,
                gint max_width = -1)
{
	int old_x = getcurx(win);

	gchar *filename_canon = String::canonicalize_ctl(filename);
	size_t filename_len = strlen(filename_canon);

	if (max_width < 0)
		max_width = getmaxx(win) - old_x;

	if (filename_len <= (size_t)max_width) {
		waddstr(win, filename_canon);
	} else {
		const gchar *keep_post = filename_canon + filename_len -
		                         max_width + 3;

#ifdef G_OS_WIN32
		const gchar *keep_pre = g_path_skip_root(filename_canon);
		if (keep_pre) {
			waddnstr(win, filename_canon,
			         keep_pre - filename_canon);
			keep_post += keep_pre - filename_canon;
		}
#endif
		wattron(win, A_UNDERLINE | A_BOLD);
		waddstr(win, "...");
		wattroff(win, A_UNDERLINE | A_BOLD);
		waddstr(win, keep_post);
	}

	g_free(filename_canon);
	return getcurx(win) - old_x;
}

void
ViewCurses::initialize_impl(void)
{
	sci = scintilla_new(scintilla_notify);
	setup();
}

InterfaceCurses::InterfaceCurses() : stdout_orig(-1), stderr_orig(-1),
                                     screen(NULL),
                                     screen_tty(NULL),
                                     info_window(NULL),
                                     info_type(INFO_TYPE_BUFFER),
                                     info_current(NULL),
                                     msg_window(NULL),
                                     cmdline_window(NULL), cmdline_pad(NULL),
                                     cmdline_len(0), cmdline_rubout_len(0)
{
	for (guint i = 0; i < G_N_ELEMENTS(color_table); i++)
		color_table[i] = -1;
	for (guint i = 0; i < G_N_ELEMENTS(orig_color_table); i++)
		orig_color_table[i].r = -1;
}

void
InterfaceCurses::Popup::add(PopupEntryType type,
                            const gchar *name, bool highlight)
{
	size_t name_len = strlen(name);
	Entry *entry = (Entry *)g_malloc(sizeof(Entry) + name_len + 1);

	entry->type = type;
	entry->highlight = highlight;
	strcpy(entry->name, name);

	longest = MAX(longest, (gint)name_len);
	length++;

	/*
	 * Entries are added in reverse (constant time for GSList),
	 * so they will later have to be reversed.
	 */
	list = g_slist_prepend(list, entry);
}

void
InterfaceCurses::Popup::init_pad(attr_t attr)
{
	int cols = getmaxx(stdscr);	/* screen width */
	int pad_lines;			/* pad height */
	gint pad_cols;			/* entry columns */
	gint pad_colwidth;		/* width per entry column */

	gint cur_col;

	/* reserve 2 spaces between columns */
	pad_colwidth = MIN(longest + 2, cols - 2);

	/* pad_cols = floor((cols - 2) / pad_colwidth) */
	pad_cols = (cols - 2) / pad_colwidth;
	/* pad_lines = ceil(length / pad_cols) */
	pad_lines = (length+pad_cols-1) / pad_cols;

	/*
	 * Render the entire autocompletion list into a pad
	 * which can be higher than the physical screen.
	 * The pad uses two columns less than the screen since
	 * it will be drawn into the popup window which has left
	 * and right borders.
	 */
	pad = newpad(pad_lines, cols - 2);

	wbkgd(pad, ' ' | attr);

	/*
	 * cur_col is the row currently written.
	 * It does not wrap but grows indefinitely.
	 * Therefore the real current row is (cur_col % popup_cols)
	 */
	cur_col = 0;
	for (GSList *cur = list; cur != NULL; cur = g_slist_next(cur)) {
		Entry *entry = (Entry *)cur->data;
		gint cur_line = cur_col/pad_cols + 1;

		wmove(pad, cur_line-1,
		      (cur_col % pad_cols)*pad_colwidth);

		wattrset(pad, entry->highlight ? A_BOLD : A_NORMAL);

		switch (entry->type) {
		case POPUP_FILE:
		case POPUP_DIRECTORY:
			format_filename(pad, entry->name);
			break;
		default:
			format_str(pad, entry->name);
			break;
		}

		cur_col++;
	}
}

void
InterfaceCurses::Popup::show(attr_t attr)
{
	int lines, cols; /* screen dimensions */
	gint pad_lines;
	gint popup_lines;
	gint bar_height, bar_y;

	if (!length)
		/* nothing to display */
		return;

	getmaxyx(stdscr, lines, cols);

	if (window)
		delwin(window);
	else
		/* reverse list only once */
		list = g_slist_reverse(list);

	if (!pad)
		init_pad(attr);
	pad_lines = getmaxy(pad);

	/*
	 * Popup window can cover all but one screen row.
	 * Another row is reserved for the top border.
	 */
	popup_lines = MIN(pad_lines + 1, lines - 1);

	/* window covers message, scintilla and info windows */
	window = newwin(popup_lines, 0, lines - 1 - popup_lines, 0);

	wbkgdset(window, ' ' | attr);

	wborder(window,
	        ACS_VLINE,
	        ACS_VLINE,	/* may be overwritten with scrollbar */
	        ACS_HLINE,
	        ' ',		/* no bottom line */
	        ACS_ULCORNER, ACS_URCORNER,
	        ACS_VLINE, ACS_VLINE);

	copywin(pad, window,
	        pad_first_line, 0,
	        1, 1, popup_lines - 1, cols - 2, FALSE);

	if (pad_lines <= popup_lines - 1)
		/* no need for scrollbar */
		return;

	/* bar_height = ceil((popup_lines-1)/pad_lines * (popup_lines-2)) */
	bar_height = ((popup_lines-1)*(popup_lines-2) + pad_lines-1) /
	             pad_lines;
	/* bar_y = floor(pad_first_line/pad_lines * (popup_lines-2)) + 1 */
	bar_y = pad_first_line*(popup_lines-2) / pad_lines + 1;

	mvwvline(window, 1, cols-1, ACS_CKBOARD, popup_lines-2);
	/*
	 * We do not use ACS_BLOCK here since it will not
	 * always be drawn as a solid block (e.g. xterm).
	 * Instead, simply draw reverse blanks.
	 */
	wmove(window, bar_y, cols-1);
	wattron(window, A_REVERSE);
	wvline(window, ' ', bar_height);

	/* progress scroll position */
	pad_first_line += popup_lines - 1;
	/* wrap on last shown page */
	pad_first_line %= pad_lines;
	if (pad_lines - pad_first_line < popup_lines - 1)
		/* show last page */
		pad_first_line = pad_lines - (popup_lines - 1);
}

void
InterfaceCurses::Popup::clear(void)
{
	g_slist_free_full(list, g_free);
	list = NULL;
	length = 0;
	longest = 0;

	pad_first_line = 0;

	if (window) {
		delwin(window);
		window = NULL;
	}

	if (pad) {
		delwin(pad);
		pad = NULL;
	}
}

InterfaceCurses::Popup::~Popup()
{
	if (window)
		delwin(window);
	if (pad)
		delwin(pad);
	if (list)
		g_slist_free_full(list, g_free);
}

void
InterfaceCurses::main_impl(int &argc, char **&argv)
{
	/*
	 * We must register this handler to handle
	 * asynchronous interruptions via CTRL+C
	 * reliably. The signal handler we already
	 * have won't do.
	 */
#if defined(PDCURSES_WIN32) || defined(NCURSES_WIN32)
	SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#endif

	/*
	 * Make sure we have a string for the info line
	 * even if info_update() is never called.
	 */
	info_current = g_strdup(PACKAGE_NAME);
}

void
InterfaceCurses::init_color_safe(guint color, guint32 rgb)
{
	short r, g, b;

#ifdef PDCURSES_WIN32
	if (orig_color_table[color].r < 0) {
		color_content((short)color,
		              &orig_color_table[color].r,
		              &orig_color_table[color].g,
		              &orig_color_table[color].b);
	}
#endif

	rgb2curses(rgb, r, g, b);
	::init_color((short)color, r, g, b);
}

#ifdef PDCURSES_WIN32

/*
 * On PDCurses/win32, color_content() will actually return
 * the real console color palette - or at least the default
 * palette when the console started.
 */
void
InterfaceCurses::restore_colors(void)
{
	if (!can_change_color())
		return;

	for (guint i = 0; i < G_N_ELEMENTS(orig_color_table); i++) {
		if (orig_color_table[i].r < 0)
			continue;

		::init_color((short)i,
		             orig_color_table[i].r,
		             orig_color_table[i].g,
		             orig_color_table[i].b);
	}
}

#elif defined(NCURSES_UNIX)

/*
 * FIXME: On UNIX/ncurses init_color_safe() __may__ change the
 * terminal's palette permanently and there does not appear to be
 * any portable way of restoring the original one.
 * Curses has color_content(), but there is actually no terminal
 * that allows querying the current palette and so color_content()
 * will return bogus "default" values and only for the first 8 colors.
 * It would do more damage to restore the palette returned by
 * color_content() than it helps.
 * xterm has the escape sequence "\e]104\x07" which restores
 * the palette from Xdefaults but not all terminal emulators
 * claiming to be "xterm" via $TERM support this escape sequence.
 * lxterminal for instance will print gibberish instead.
 * So we try to look whether $XTERM_VERSION is set.
 * There are hardly any other terminal emulators that support palette
 * resets.
 * The only emulator I'm aware of which can be identified reliably
 * by $TERM supporting a palette reset is the Linux console
 * (see console_codes(4)). The escape sequence "\e]R" is already
 * part of its terminfo description (orig_colors capability)
 * which is apparently sent by endwin(), so the palette is
 * already properly restored on endwin().
 * Welcome in Curses hell.
 */
void
InterfaceCurses::restore_colors(void)
{
	if (g_str_has_prefix(g_getenv("TERM") ? : "", "xterm") &&
	    g_getenv("XTERM_VERSION")) {
		/*
		 * Looks like a real xterm. $TERM alone is not
		 * sufficient to tell.
		 */
		fputs("\e]104\x07", screen_tty);
		fflush(screen_tty);
	}
}

#else /* !PDCURSES_WIN32 && !NCURSES_UNIX */

void
InterfaceCurses::restore_colors(void)
{
	/*
	 * No way to restore the palette, or it's
	 * unnecessary (e.g. XCurses)
	 */
}

#endif

void
InterfaceCurses::init_color(guint color, guint32 rgb)
{
	if (color >= G_N_ELEMENTS(color_table))
		return;

#if defined(__PDCURSES__) && !defined(PDC_RGB)
	/*
	 * PDCurses will usually number color codes differently
	 * (least significant bit is the blue component) while
	 * SciTECO macros will assume a standard terminal color
	 * code numbering with red as the LSB.
	 * Therefore we have to swap the bit order of the least
	 * significant 3 bits here.
	 */
	color = (color & ~0x5) |
	        ((color & 0x1) << 2) | ((color & 0x4) >> 2);
#endif

	if (cmdline_window) {
		/* interactive mode */
		if (!can_change_color())
			return;

		init_color_safe(color, rgb);
	} else {
		/*
		 * batch mode: store colors,
		 * they can only be initialized after start_color()
		 * which is called by Scinterm when interactive
		 * mode is initialized
		 */
		color_table[color] = (gint32)rgb;
	}
}

#ifdef NCURSES_UNIX

void
InterfaceCurses::init_screen(void)
{
	screen_tty = g_fopen("/dev/tty", "r+");
	/* should never fail */
	g_assert(screen_tty != NULL);

	screen = newterm(NULL, screen_tty, screen_tty);
	if (!screen) {
		g_fprintf(stderr, "Error initializing interactive mode. "
		                  "$TERM may be incorrect.\n");
		exit(EXIT_FAILURE);
	}

	/*
	 * If stdout or stderr would go to the terminal,
	 * redirect it. Otherwise, they are already redirected
	 * (e.g. to a file) and writing to them does not
	 * interrupt terminal interaction.
	 */
	if (isatty(1)) {
		FILE *stdout_new;
		stdout_orig = dup(1);
		g_assert(stdout_orig >= 0);
		stdout_new = g_freopen("/dev/null", "a+", stdout);
		g_assert(stdout_new != NULL);
	}
	if (isatty(2)) {
		FILE *stderr_new;
		stderr_orig = dup(2);
		g_assert(stderr_orig >= 0);
		stderr_new = g_freopen("/dev/null", "a+", stderr);
		g_assert(stderr_new != NULL);
	}
}

#elif defined(XCURSES)

void
InterfaceCurses::init_screen(void)
{
	const char *argv[] = {PACKAGE_NAME, NULL};

	/*
	 * This sets the program name to "SciTECO"
	 * which may then also be used as the X11 class name
	 * for overwriting X11 resources in .Xdefaults
	 * FIXME: We could support passing in resource
	 * overrides via the SciTECO command line.
	 * But unfortunately, Xinitscr() is called too
	 * late to modify argc/argv for command-line parsing.
	 * Therefore this could only be supported by
	 * adding a special option like --resource.
	 */
	Xinitscr(1, (char **)argv);
}

#else

void
InterfaceCurses::init_screen(void)
{
	initscr();
}

#endif

void
InterfaceCurses::init_interactive(void)
{
	/*
	 * Curses accesses many environment variables
	 * internally. In order to be able to modify them in
	 * the SciTECO profile, we must update the process
	 * environment before initscr()/newterm().
	 * This is safe to do here since there are no threads.
	 */
	QRegisters::globals.update_environ();

	/*
	 * On UNIX terminals, the escape key is usually
	 * delivered as the escape character even though function
	 * keys are delivered as escape sequences as well.
	 * That's why there has to be a timeout for detecting
	 * escape presses if function key handling is enabled.
	 * This timeout can be controlled using $ESCDELAY on
	 * ncurses but its default is much too long.
	 * We set it to 25ms as Vim does. In the very rare cases
	 * this won't suffice, $ESCDELAY can still be set explicitly.
	 *
	 * NOTE: The only terminal emulator I'm aware of that lets
	 * us send an escape sequence for the escape key is Mintty
	 * (see "\e[?7727h").
	 */
#ifdef NCURSES_UNIX
	if (!g_getenv("ESCDELAY"))
		set_escdelay(25);
#endif

	/*
	 * $TERM must be unset or "#win32con" for the win32
	 * driver to load.
	 * So we always ignore any $TERM changes by the user.
	 */
#ifdef NCURSES_WIN32
	g_setenv("TERM", "#win32con", TRUE);
#endif

#ifdef PDCURSES_WIN32A
	/*
	 * Necessary to enable window resizing in Win32a port
	 */
	PDC_set_resize_limits(25, 0xFFFF, 80, 0xFFFF);

	/*
	 * Disable all magic function keys.
	 * NOTE: This could also be used to assign
	 * a "shutdown" key when program termination is requested.
	 */
	for (int i = 0; i < N_FUNCTION_KEYS; i++)
		PDC_set_function_key(i, 0);

	/*
	 * Register the special shutdown function with the
	 * CLOSE key, so closing the window behaves similar as on
	 * GTK+.
	 */
	PDC_set_function_key(FUNCTION_KEY_SHUT_DOWN, KEY_CLOSE);
#endif

	/* for displaying UTF-8 characters properly */
	setlocale(LC_CTYPE, "");

	init_screen();

	cbreak();
	noecho();
	/* Scintilla draws its own cursor */
	curs_set(0);

	info_window = newwin(1, 0, 0, 0);

	msg_window = newwin(1, 0, LINES - 2, 0);

	cmdline_window = newwin(0, 0, LINES - 1, 0);
	keypad(cmdline_window, TRUE);

#ifdef EMSCRIPTEN
        nodelay(cmdline_window, TRUE);
#endif

	/*
	 * Will also initialize Scinterm, Curses color pairs
	 * and resizes the current view.
	 */
	if (current_view)
		show_view(current_view);

	/*
	 * Only now it's safe to redefine the 16 default colors.
	 */
	if (can_change_color()) {
		for (guint i = 0; i < G_N_ELEMENTS(color_table); i++) {
			/*
			 * init_color() may still fail if COLORS < 16
			 */
			if (color_table[i] >= 0)
				init_color_safe(i, (guint32)color_table[i]);
		}
	}
}

void
InterfaceCurses::restore_batch(void)
{
	/*
	 * Set window title to a reasonable default,
	 * in case it is not reset immediately by the
	 * shell.
	 * FIXME: See set_window_title() why this
	 * is necessary.
	 */
#if defined(NCURSES_UNIX) && defined(HAVE_TIGETSTR)
	set_window_title(g_getenv("TERM") ? : "");
#endif

	/*
	 * Restore ordinary terminal behaviour
	 * (i.e. return to batch mode)
	 */
	endwin();
	restore_colors();

	/*
	 * Restore stdout and stderr, so output goes to
	 * the terminal again in case we "muted" them.
	 */
#ifdef NCURSES_UNIX
	if (stdout_orig >= 0) {
		int fd = dup2(stdout_orig, 1);
		g_assert(fd == 1);
	}
	if (stderr_orig >= 0) {
		int fd = dup2(stderr_orig, 2);
		g_assert(fd == 2);
	}
#endif

	/*
	 * See vmsg_impl(): It looks at msg_win to determine
	 * whether we're in batch mode.
	 */
	if (msg_window) {
		delwin(msg_window);
		msg_window = NULL;
	}
}

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
	short fg, bg;

	if (!msg_window) { /* batch mode */
		stdio_vmsg(type, fmt, ap);
		return;
	}

	/*
	 * On most platforms we can write to stdout/stderr
	 * even in interactive mode.
	 */
#if defined(XCURSES) || defined(PDCURSES_WIN32A) || \
    defined(NCURSES_UNIX) || defined(NCURSES_WIN32)
	va_list aq;
	va_copy(aq, ap);
	stdio_vmsg(type, fmt, aq);
	va_end(aq);
#endif

	fg = rgb2curses(ssm(SCI_STYLEGETBACK, STYLE_DEFAULT));

	switch (type) {
	default:
	case MSG_USER:
		bg = rgb2curses(ssm(SCI_STYLEGETFORE, STYLE_DEFAULT));
		break;
	case MSG_INFO:
		bg = COLOR_GREEN;
		break;
	case MSG_WARNING:
		bg = COLOR_YELLOW;
		break;
	case MSG_ERROR:
		bg = COLOR_RED;
		beep();
		break;
	}

	wmove(msg_window, 0, 0);
	wbkgdset(msg_window, ' ' | SCI_COLOR_ATTR(fg, bg));
	vw_printw(msg_window, fmt, ap);
	wclrtoeol(msg_window);
}

void
InterfaceCurses::msg_clear(void)
{
	short fg, bg;

	if (!msg_window) /* batch mode */
		return;

	fg = rgb2curses(ssm(SCI_STYLEGETBACK, STYLE_DEFAULT));
	bg = rgb2curses(ssm(SCI_STYLEGETFORE, STYLE_DEFAULT));

	wbkgdset(msg_window, ' ' | SCI_COLOR_ATTR(fg, bg));
	werase(msg_window);
}

void
InterfaceCurses::show_view_impl(ViewCurses *view)
{
	int lines, cols; /* screen dimensions */
	WINDOW *current_view_win;

	current_view = view;

	if (!cmdline_window) /* batch mode */
		return;

	current_view_win = current_view->get_window();

	/*
	 * screen size might have changed since
	 * this view's WINDOW was last active
	 */
	getmaxyx(stdscr, lines, cols);
	wresize(current_view_win, lines - 3, cols);
	/* Set up window position: never changes */
	mvwin(current_view_win, 1, 0);
}

#if PDCURSES

void
InterfaceCurses::set_window_title(const gchar *title)
{
	static gchar *last_title = NULL;

	/*
	 * PDC_set_title() can result in flickering
	 * even when executed only once per pressed key,
	 * so we check whether it is really necessary to change
	 * the title.
	 * This is an issue at least with PDCurses/win32.
	 */
	if (!g_strcmp0(title, last_title))
		return;

	PDC_set_title(title);

	g_free(last_title);
	last_title = g_strdup(title);
}

#elif defined(NCURSES_UNIX) && defined(HAVE_TIGETSTR)

void
InterfaceCurses::set_window_title(const gchar *title)
{
	if (!has_status_line || !to_status_line || !from_status_line)
		return;

	/*
	 * Modern terminal emulators map the window title to
	 * the historic status line.
	 * This feature is not standardized in ncurses,
	 * so we query the terminfo database.
	 * This feature may make problems with terminal emulators
	 * that do support a status line but do not map them
	 * to the window title. Some emulators (like xterm)
	 * support setting the window title via custom escape
	 * sequences and via the status line but their
	 * terminfo entry does not say so. (xterm can also
	 * save and restore window titles but there is not
	 * even a terminfo capability defined for this.)
	 * Taken the different emulator incompatibilites
	 * it may be best to make this configurable.
	 * Once we support configurable status lines,
	 * there could be a special status line that's sent
	 * to the terminal that may be set up in the profile
	 * depending on $TERM.
	 *
	 * NOTE: The terminfo manpage advises us to use putp()
	 * but on ncurses/UNIX (where terminfo is available),
	 * we do not let curses write to stdout.
	 * NOTE: This leaves the title set after we quit.
	 */
	fputs(to_status_line, screen_tty);
	fputs(title, screen_tty);
	fputs(from_status_line, screen_tty);
	fflush(screen_tty);
}

#else

void
InterfaceCurses::set_window_title(const gchar *title)
{
	/* no way to set window title */
}

#endif

void
InterfaceCurses::draw_info(void)
{
	short fg, bg;
	const gchar *info_type_str;
	gchar *info_current_canon, *title;

	if (!info_window) /* batch mode */
		return;

	/*
	 * The info line is printed in reverse colors of
	 * the current buffer's STYLE_DEFAULT.
	 * The same style is used for MSG_USER messages.
	 */
	fg = rgb2curses(ssm(SCI_STYLEGETBACK, STYLE_DEFAULT));
	bg = rgb2curses(ssm(SCI_STYLEGETFORE, STYLE_DEFAULT));

	wmove(info_window, 0, 0);
	wbkgdset(info_window, ' ' | SCI_COLOR_ATTR(fg, bg));

	switch (info_type) {
	case INFO_TYPE_QREGISTER:
		info_type_str = PACKAGE_NAME " - <QRegister> ";
		waddstr(info_window, info_type_str);
		/* same formatting as in command lines */
		format_str(info_window, info_current);
		break;

	case INFO_TYPE_BUFFER:
		info_type_str = PACKAGE_NAME " - <Buffer> ";
		waddstr(info_window, info_type_str);
		format_filename(info_window, info_current);
		break;

	default:
		g_assert_not_reached();
	}

	wclrtoeol(info_window);

	/*
	 * Make sure the title will consist only of printable
	 * characters
	 */
	info_current_canon = String::canonicalize_ctl(info_current);
	title = g_strconcat(info_type_str, info_current_canon, NIL);
	g_free(info_current_canon);
	set_window_title(title);
	g_free(title);
}

void
InterfaceCurses::info_update_impl(const QRegister *reg)
{
	g_free(info_current);
	/* NOTE: will contain control characters */
	info_type = INFO_TYPE_QREGISTER;
	info_current = g_strdup(reg->name);
	/* NOTE: drawn in event_loop_iter() */
}

void
InterfaceCurses::info_update_impl(const Buffer *buffer)
{
	g_free(info_current);
	info_type = INFO_TYPE_BUFFER;
	info_current = g_strconcat(buffer->filename ? : UNNAMED_FILE,
	                           buffer->dirty ? "*" : " ", NIL);
	/* NOTE: drawn in event_loop_iter() */
}

void
InterfaceCurses::cmdline_update_impl(const Cmdline *cmdline)
{
	short fg, bg;
	int max_cols = 1;

	/*
	 * Replace entire pre-formatted command-line.
	 * We don't know if it is similar to the last one,
	 * so resizing makes no sense.
	 * We approximate the size of the new formatted command-line,
	 * wasting a few bytes for control characters.
	 */
	if (cmdline_pad)
		delwin(cmdline_pad);
	for (guint i = 0; i < cmdline->len+cmdline->rubout_len; i++)
		max_cols += IS_CTL((*cmdline)[i]) ? 3 : 1;
	cmdline_pad = newpad(1, max_cols);

	fg = rgb2curses(ssm(SCI_STYLEGETFORE, STYLE_DEFAULT));
	bg = rgb2curses(ssm(SCI_STYLEGETBACK, STYLE_DEFAULT));
	wcolor_set(cmdline_pad, SCI_COLOR_PAIR(fg, bg), NULL);

	/* format effective command line */
	cmdline_len = format_str(cmdline_pad, cmdline->str, cmdline->len);

	/*
	 * A_BOLD should result in either a bold font or a brighter
	 * color both on 8 and 16 color terminals.
	 * This is not quite color-scheme-agnostic, but works
	 * with both the `terminal` and `solarized` themes.
	 * This problem will be gone once we use a Scintilla view
	 * as command line, since we can then define a style
	 * for rubbed out parts of the command line which will
	 * be user-configurable.
	 */
	wattron(cmdline_pad, A_UNDERLINE | A_BOLD);

	/*
	 * Format rubbed-out command line.
	 * NOTE: This formatting will never be truncated since we're
	 * writing into the pad which is large enough.
	 */
	cmdline_rubout_len = format_str(cmdline_pad, cmdline->str + cmdline->len,
	                                cmdline->rubout_len);

	/* highlight cursor after effective command line */
	if (cmdline_rubout_len) {
		attr_t attr;
		short pair;

		wmove(cmdline_pad, 0, cmdline_len);
		wattr_get(cmdline_pad, &attr, &pair, NULL);
		wchgat(cmdline_pad, 1,
		       (attr & A_UNDERLINE) | A_REVERSE, pair, NULL);
	} else {
		cmdline_len++;
		wattroff(cmdline_pad, A_UNDERLINE | A_BOLD);
		waddch(cmdline_pad, ' ' | A_REVERSE);
	}

	draw_cmdline();
}

void
InterfaceCurses::draw_cmdline(void)
{
	short fg, bg;
	/* total width available for command line */
	guint total_width = getmaxx(cmdline_window) - 1;
	/* beginning of command line to show */
	guint disp_offset;
	/* length of command line to show */
	guint disp_len;

	disp_offset = cmdline_len -
	              MIN(cmdline_len,
	                  total_width/2 + cmdline_len % MAX(total_width/2, 1));
	/*
	 * NOTE: we do not use getmaxx(cmdline_pad) here since it may be
	 * larger than the text the pad contains.
	 */
	disp_len = MIN(total_width, cmdline_len+cmdline_rubout_len - disp_offset);

	fg = rgb2curses(ssm(SCI_STYLEGETFORE, STYLE_DEFAULT));
	bg = rgb2curses(ssm(SCI_STYLEGETBACK, STYLE_DEFAULT));

	wbkgdset(cmdline_window, ' ' | SCI_COLOR_ATTR(fg, bg));
	werase(cmdline_window);
	mvwaddch(cmdline_window, 0, 0, '*' | A_BOLD);
	copywin(cmdline_pad, cmdline_window,
	        0, disp_offset, 0, 1, 0, disp_len, FALSE);
}

void
InterfaceCurses::popup_show_impl(void)
{
	short fg, bg;

	if (!cmdline_window)
		/* batch mode */
		return;

	fg = rgb2curses(ssm(SCI_STYLEGETFORE, STYLE_CALLTIP));
	bg = rgb2curses(ssm(SCI_STYLEGETBACK, STYLE_CALLTIP));

	popup.show(SCI_COLOR_ATTR(fg, bg));
}

void
InterfaceCurses::popup_clear_impl(void)
{
#ifdef __PDCURSES__
	/*
	 * PDCurses will not redraw all windows that may be
	 * overlapped by the popup window correctly - at least
	 * not the info window.
	 * The Scintilla window is apparently always touched by
	 * scintilla_noutrefresh().
	 * Actually we would expect this to be necessary on any curses,
	 * but ncurses doesn't require this.
	 */
	if (popup.is_shown()) {
		touchwin(info_window);
		touchwin(msg_window);
	}
#endif

	popup.clear();
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
	 * On PDCurses/win32, raw() and cbreak() does
	 * not disable and enable CTRL+C handling properly.
	 * Since I don't want to patch PDCurses/win32,
	 * we do this manually here.
	 * NOTE: This exploits the fact that PDCurses uses
	 * STD_INPUT_HANDLE internally!
	 */
#ifdef PDCURSES_WIN32
	HANDLE console_hnd = GetStdHandle(STD_INPUT_HANDLE);
	DWORD console_mode;
	GetConsoleMode(console_hnd, &console_mode);
#endif

	/*
	 * Setting function key processing is important
	 * on Unix Curses, as ESCAPE is handled as the beginning
	 * of a escape sequence when terminal emulators are
	 * involved.
	 * On some Curses variants (XCurses) however, keypad
	 * must always be TRUE so we receive KEY_RESIZE.
	 */
#ifdef NCURSES_UNIX
	keypad(interface.cmdline_window, Flags::ed & Flags::ED_FNKEYS);
#endif

	/* no special <CTRL/C> handling */
	raw();
#ifdef PDCURSES_WIN32
	SetConsoleMode(console_hnd, console_mode & ~ENABLE_PROCESSED_INPUT);
#endif
	key = wgetch(interface.cmdline_window);
	/* allow asynchronous interruptions on <CTRL/C> */
	sigint_occurred = FALSE;
	noraw(); /* FIXME: necessary because of NCURSES_WIN32 bug */
	cbreak();
#ifdef PDCURSES_WIN32
	SetConsoleMode(console_hnd, console_mode | ENABLE_PROCESSED_INPUT);
#endif
	if (key == ERR)
		return;

	switch (key) {
#ifdef KEY_RESIZE
	case KEY_RESIZE:
#if PDCURSES
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
		cmdline.keypress('\n');
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
	FN(CLOSE);
#undef FNS
#undef FN

	/*
	 * Control keys and keys with printable representation
	 */
	default:
		if (key <= 0xFF)
			cmdline.keypress((gchar)key);
	}

	/*
	 * Info window is updated very often which is very
	 * costly, especially when using PDC_set_title(),
	 * so we redraw it here, where the overhead does
	 * not matter much.
	 */
	interface.draw_info();
	wnoutrefresh(interface.info_window);
	interface.current_view->noutrefresh();
	wnoutrefresh(interface.msg_window);
	wnoutrefresh(interface.cmdline_window);
	interface.popup.noutrefresh();
	doupdate();
}

void
InterfaceCurses::event_loop_impl(void)
{
	static const Cmdline empty_cmdline;

	/*
	 * Initialize Curses for interactive mode
	 */
	init_interactive();

	/* initial refresh */
	draw_info();
	wnoutrefresh(info_window);
	current_view->noutrefresh();
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

	restore_batch();
#endif
}

InterfaceCurses::~InterfaceCurses()
{
	if (info_window)
		delwin(info_window);
	g_free(info_current);
	if (cmdline_window)
		delwin(cmdline_window);
	if (cmdline_pad)
		delwin(cmdline_pad);
	if (msg_window)
		delwin(msg_window);

	/*
	 * PDCurses (win32) crashes if initscr() wasn't called.
	 * Others (XCurses) crash if we try to use isendwin() here.
	 * Perhaps Curses cleanup should be in restore_batch()
	 * instead.
	 */
#ifndef XCURSES
	if (info_window && !isendwin())
		endwin();
#endif

	if (screen)
		delscreen(screen);
	if (screen_tty)
		fclose(screen_tty);
	if (stderr_orig >= 0)
		close(stderr_orig);
	if (stdout_orig >= 0)
		close(stdout_orig);
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
