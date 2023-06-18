/*
 * Copyright (C) 2012-2023 Robin Haberkorn
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

#ifdef HAVE_WINDOWS_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/*
 * Some macros in wincon.h interfere with our code.
 */
#undef MOUSE_MOVED
#endif

#ifdef EMSCRIPTEN
#include <emscripten.h>
#endif

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
#include <ScintillaCurses.h>

#include "sciteco.h"
#include "string-utils.h"
#include "cmdline.h"
#include "qreg.h"
#include "ring.h"
#include "error.h"
#include "curses-utils.h"
#include "curses-info-popup.h"
#include "view.h"
#include "memory.h"
#include "interface.h"

#if defined(__PDCURSES__) && defined(G_OS_WIN32) && \
    !defined(PDCURSES_GUI)
#define PDCURSES_WINCON
#endif

/**
 * Whether we're on EMCurses.
 * Could be replaced with a configure-time check for
 * PDC_emscripten_set_handler().
 */
#if defined(__PDCURSES__) && defined(EMSCRIPTEN)
#define EMCURSES
#endif

#ifdef NCURSES_VERSION
#ifdef G_OS_UNIX
/** Whether we're on ncurses/UNIX. */
#define NCURSES_UNIX
#elif defined(G_OS_WIN32)
/** Whether we're on ncurses/win32 console */
#define NCURSES_WIN32
#endif
#endif

#if defined(NCURSES_UNIX) || defined(NETBSD_CURSES)
/**
 * Whether Curses works on a real or pseudo TTY
 * (ie. classic use with terminal emulators on Unix)
 */
#define CURSES_TTY
#endif

#ifdef G_OS_WIN32

/**
 * This handler is the Windows-analogue of a signal
 * handler. MinGW provides signal(), but it's not
 * reliable.
 * This may also be used to handle CTRL_CLOSE_EVENTs.
 *
 * NOTE: Unlike signal handlers, this is executed in a
 * separate thread.
 */
static BOOL WINAPI
teco_console_ctrl_handler(DWORD type)
{
	switch (type) {
	case CTRL_C_EVENT:
		teco_interrupted = TRUE;
		return TRUE;
	}

	return FALSE;
}

#endif

static gint teco_xterm_version(void) G_GNUC_UNUSED;

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
 * Returns the curses `COLOR_PAIR` for the given curses foreground and background `COLOR`s.
 * This is used simply to enumerate every possible color combination.
 * Note: only 256 combinations are possible due to curses portability.
 * Note: This references the global curses variable `COLORS` and is not a constant expression.
 * @param f The curses foreground `COLOR`.
 * @param b The curses background `COLOR`.
 * @return int number for defining a curses `COLOR_PAIR`.
 */
#define SCI_COLOR_PAIR(f, b) ((b) * ((COLORS < 16) ? 8 : 16) + (f) + 1)

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
teco_rgb2curses_triple(guint32 rgb, gshort *r, gshort *g, gshort *b)
{
	/* NOTE: We could also use 200/51 */
	*r = ((rgb & 0x0000FF) >> 0)*1000/0xFF;
	*g = ((rgb & 0x00FF00) >> 8)*1000/0xFF;
	*b = ((rgb & 0xFF0000) >> 16)*1000/0xFF;
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
static gshort
teco_rgb2curses(guint32 rgb)
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

static gint
teco_xterm_version(void)
{
	static gint xterm_patch = -2;

	/*
	 * The XTerm patch level (version) is cached.
	 */
	if (G_LIKELY(xterm_patch != -2))
		return xterm_patch;
	xterm_patch = -1;

	const gchar *term = g_getenv("TERM");

	if (!term || !g_str_has_prefix(term, "xterm"))
		/* no XTerm */
		return -1;

	/*
	 * Terminal might claim to be XTerm-compatible,
	 * but this only refers to the terminfo database.
	 * XTERM_VERSION however should be sufficient to tell
	 * whether we are running under a real XTerm.
	 */
	const gchar *xterm_version = g_getenv("XTERM_VERSION");
	if (!xterm_version)
		/* no XTerm */
		return -1;
	xterm_patch = 0;

	xterm_version = strrchr(xterm_version, '(');
	if (!xterm_version)
		/* Invalid XTERM_VERSION, assume some XTerm */
		return 0;

	xterm_patch = atoi(xterm_version+1);
	return xterm_patch;
}

/*
 * NOTE: The teco_view_t pointer is reused to directly
 * point to the Scintilla object.
 * This saves one heap object per view.
 */

static void
teco_view_scintilla_notify(void *sci, int iMessage, SCNotification *notify, void *user_data)
{
	teco_interface_process_notify(notify);
}

teco_view_t *
teco_view_new(void)
{
	return (teco_view_t *)scintilla_new(teco_view_scintilla_notify, NULL);
}

static inline void
teco_view_noutrefresh(teco_view_t *ctx)
{
	scintilla_noutrefresh(ctx);
}

static inline WINDOW *
teco_view_get_window(teco_view_t *ctx)
{
	return scintilla_get_window(ctx);
}

sptr_t
teco_view_ssm(teco_view_t *ctx, unsigned int iMessage, uptr_t wParam, sptr_t lParam)
{
	return scintilla_send_message(ctx, iMessage, wParam, lParam);
}

void
teco_view_free(teco_view_t *ctx)
{
	scintilla_delete(ctx);
}

static struct {
	/**
	 * Mapping of the first 16 curses color codes (that may or may not
	 * correspond with the standard terminal color codes) to
	 * Scintilla-compatible RGB values (red is LSB) to initialize after
	 * Curses startup.
	 * Negative values mean no color redefinition (keep the original
	 * palette entry).
	 */
	gint32 color_table[16];

	/**
	 * Mapping of the first 16 curses color codes to their
	 * original values for restoring them on shutdown.
	 * Unfortunately, this may not be supported on all
	 * curses ports, so this array may be unused.
	 */
	struct {
		gshort r, g, b;
	} orig_color_table[16];

	int stdout_orig, stderr_orig;
	SCREEN *screen;
	FILE *screen_tty;

	WINDOW *info_window;
	enum {
		TECO_INFO_TYPE_BUFFER = 0,
		TECO_INFO_TYPE_QREG
	} info_type;
	teco_string_t info_current;

	WINDOW *msg_window;

	WINDOW *cmdline_window, *cmdline_pad;
	gsize cmdline_len, cmdline_rubout_len;

	GQueue *input_queue;

	teco_curses_info_popup_t popup;

	/**
	 * GError "thrown" by teco_interface_event_loop_iter().
	 * Having this in a variable avoids problems with EMScripten.
	 */
	GError *event_loop_error;
} teco_interface;

static void teco_interface_init_color_safe(guint color, guint32 rgb);
static void teco_interface_restore_colors(void);

static void teco_interface_init_screen(void);
static gboolean teco_interface_init_interactive(GError **error);
static void teco_interface_restore_batch(void);

static void teco_interface_init_clipboard(void);

static void teco_interface_resize_all_windows(void);

static void teco_interface_set_window_title(const gchar *title);
static void teco_interface_draw_info(void);
static void teco_interface_draw_cmdline(void);

void
teco_interface_init(void)
{
	for (guint i = 0; i < G_N_ELEMENTS(teco_interface.color_table); i++)
		teco_interface.color_table[i] = -1;
	for (guint i = 0; i < G_N_ELEMENTS(teco_interface.orig_color_table); i++)
		teco_interface.orig_color_table[i].r = -1;

	teco_interface.stdout_orig = teco_interface.stderr_orig = -1;

	teco_curses_info_popup_init(&teco_interface.popup);	

	/*
	 * Make sure we have a string for the info line
	 * even if teco_interface_info_update() is never called.
	 */
	teco_string_init(&teco_interface.info_current, PACKAGE_NAME, strlen(PACKAGE_NAME));

	/*
	 * On all platforms except Curses/XTerm, it's
	 * safe to initialize the clipboards now.
	 */
#ifndef CURSES_TTY
	teco_interface_init_clipboard();
#endif

	/*
	 * The default SIGINT signal handler seems to partially work
	 * as the console control handler.
	 * However, a second CTRL+C event (or raise(SIGINT)) would
	 * terminate the process.
	 */
#ifdef G_OS_WIN32
	SetConsoleCtrlHandler(teco_console_ctrl_handler, TRUE);
#endif
}

GOptionGroup *
teco_interface_get_options(void)
{
	return NULL;
}

static void
teco_interface_init_color_safe(guint color, guint32 rgb)
{
#if defined(__PDCURSES__) && !defined(PDCURSES_GUI)
	if (teco_interface.orig_color_table[color].r < 0) {
		color_content((short)color,
		              &teco_interface.orig_color_table[color].r,
		              &teco_interface.orig_color_table[color].g,
		              &teco_interface.orig_color_table[color].b);
	}
#endif

	gshort r, g, b;
	teco_rgb2curses_triple(rgb, &r, &g, &b);
	init_color((short)color, r, g, b);
}

#if defined(__PDCURSES__) && !defined(PDCURSES_GUI)

/*
 * On PDCurses/WinCon, color_content() will actually return
 * the real console color palette - or at least the default
 * palette when the console started.
 */
static void
teco_interface_restore_colors(void)
{
	if (!can_change_color())
		return;

	for (guint i = 0; i < G_N_ELEMENTS(teco_interface.orig_color_table); i++) {
		if (teco_interface.orig_color_table[i].r < 0)
			continue;

		init_color((short)i,
		           teco_interface.orig_color_table[i].r,
		           teco_interface.orig_color_table[i].g,
		           teco_interface.orig_color_table[i].b);
	}
}

#elif defined(CURSES_TTY)

/*
 * FIXME: On UNIX/ncurses teco_interface_init_color_safe() __may__
 * change the terminal's palette permanently and there does not
 * appear to be any portable way of restoring the original one.
 * Curses has color_content(), but there is actually no terminal
 * that allows querying the current palette and so color_content()
 * will return bogus "default" values and only for the first 8 colors.
 * It would do more damage to restore the palette returned by
 * color_content() than it helps.
 * xterm has the escape sequence "\e]104\a" which restores
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
static void
teco_interface_restore_colors(void)
{
	if (teco_xterm_version() < 0)
		return;

	/*
	 * Looks like a real XTerm
	 */
	fputs("\e]104\a", teco_interface.screen_tty);
	fflush(teco_interface.screen_tty);
}

#else /* (!__PDCURSES__ || PDCURSES_GUI) && !CURSES_TTY */

static void
teco_interface_restore_colors(void)
{
	/*
	 * No way to restore the palette, or it's
	 * unnecessary (e.g. XCurses)
	 */
}

#endif

void
teco_interface_init_color(guint color, guint32 rgb)
{
	if (color >= G_N_ELEMENTS(teco_interface.color_table))
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

	if (teco_interface.cmdline_window) {
		/* interactive mode */
		if (!can_change_color())
			return;

		teco_interface_init_color_safe(color, rgb);
	} else {
		/*
		 * batch mode: store colors,
		 * they can only be initialized after start_color()
		 * which is called by Scinterm when interactive
		 * mode is initialized
		 */
		teco_interface.color_table[color] = (gint32)rgb;
	}
}

#ifdef CURSES_TTY

static void
teco_interface_init_screen(void)
{
	teco_interface.screen_tty = g_fopen("/dev/tty", "r+");
	/* should never fail */
	g_assert(teco_interface.screen_tty != NULL);

	teco_interface.screen = newterm(NULL, teco_interface.screen_tty, teco_interface.screen_tty);
	if (!teco_interface.screen) {
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
		teco_interface.stdout_orig = dup(1);
		g_assert(teco_interface.stdout_orig >= 0);
		FILE *stdout_new = g_freopen("/dev/null", "a+", stdout);
		g_assert(stdout_new != NULL);
	}
	if (isatty(2)) {
		teco_interface.stderr_orig = dup(2);
		g_assert(teco_interface.stderr_orig >= 0);
		FILE *stderr_new = g_freopen("/dev/null", "a+", stderr);
		g_assert(stderr_new != NULL);
	}
}

#elif defined(XCURSES)

static void
teco_interface_init_screen(void)
{
	const char *argv[] = {PACKAGE_NAME, NULL};

	/*
	 * This sets the program name to "SciTECO"
	 * which may then also be used as the X11 class name
	 * for overwriting X11 resources in .Xdefaults
	 *
	 * FIXME: We could support passing in resource
	 * overrides via the SciTECO command line.
	 * But unfortunately, Xinitscr() is called too
	 * late to modify argc/argv for command-line parsing
	 * (and GOption needs to know about the additional
	 * possible arguments since they are not passed through
	 * transparently).
	 * Therefore this could only be supported by
	 * adding a special option like --resource KEY=VAL.
	 */
	Xinitscr(1, (char **)argv);
}

#else /* !CURSES_TTY && !XCURSES */

static void
teco_interface_init_screen(void)
{
	initscr();
}

#endif

static gboolean
teco_interface_init_interactive(GError **error)
{
	/*
	 * Curses accesses many environment variables
	 * internally. In order to be able to modify them in
	 * the SciTECO profile, we must update the process
	 * environment before initscr()/newterm().
	 * This is safe to do here since there are no threads.
	 */
	if (!teco_qreg_table_set_environ(&teco_qreg_table_globals, error))
		return FALSE;

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
	 *
	 * FIXME: This appears to be ineffective for netbsd-curses.
	 */
#ifdef CURSES_TTY
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

#ifdef __PDCURSESMOD__
	/*
	 * Necessary to enable window resizing in WinGUI port
	 */
	PDC_set_resize_limits(25, 0xFFFF, 80, 0xFFFF);

	/*
	 * Disable all magic function keys.
	 */
	for (int i = 0; i < PDC_MAX_FUNCTION_KEYS; i++)
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

	teco_interface_init_screen();

	/*
	 * We always have a CTRL handler on Windows, but doing it
	 * here again, ensures that we have a higher precedence
	 * than the one installed by PDCurses.
	 */
#ifdef G_OS_WIN32
	SetConsoleCtrlHandler(teco_console_ctrl_handler, TRUE);
#endif

	cbreak();
	noecho();
	/* Scintilla draws its own cursor */
	curs_set(0);

	teco_interface.info_window = newwin(1, 0, 0, 0);

	teco_interface.msg_window = newwin(1, 0, LINES - 2, 0);

	teco_interface.cmdline_window = newwin(0, 0, LINES - 1, 0);
	keypad(teco_interface.cmdline_window, TRUE);
	nodelay(teco_interface.cmdline_window, TRUE);

	teco_interface.input_queue = g_queue_new();

	/*
	 * Will also initialize Scinterm, Curses color pairs
	 * and resizes the current view.
	 */
	if (teco_interface_current_view)
		teco_interface_show_view(teco_interface_current_view);

	/*
	 * Only now it's safe to redefine the 16 default colors.
	 */
	if (can_change_color()) {
		for (guint i = 0; i < G_N_ELEMENTS(teco_interface.color_table); i++) {
			/*
			 * init_color() may still fail if COLORS < 16
			 */
			if (teco_interface.color_table[i] >= 0)
				teco_interface_init_color_safe(i, (guint32)teco_interface.color_table[i]);
		}
	}

	/*
	 * Only now (in interactive mode), it's safe to initialize
	 * the clipboard Q-Registers on ncurses with a compatible terminal
	 * emulator since clipboard operations will no longer interfer
	 * with stdout.
	 */
#ifdef CURSES_TTY
	teco_interface_init_clipboard();
#endif

	return TRUE;
}

static void
teco_interface_restore_batch(void)
{
	/*
	 * Set window title to a reasonable default,
	 * in case it is not reset immediately by the
	 * shell.
	 * FIXME: See set_window_title() why this
	 * is necessary.
	 */
#if defined(CURSES_TTY) && defined(HAVE_TIGETSTR)
	teco_interface_set_window_title(g_getenv("TERM") ? : "");
#endif

	/*
	 * Restore ordinary terminal behaviour
	 * (i.e. return to batch mode)
	 */
	endwin();
	teco_interface_restore_colors();

	/*
	 * Restore stdout and stderr, so output goes to
	 * the terminal again in case we "muted" them.
	 */
#ifdef CURSES_TTY
	if (teco_interface.stdout_orig >= 0) {
		int fd = dup2(teco_interface.stdout_orig, 1);
		g_assert(fd == 1);
	}
	if (teco_interface.stderr_orig >= 0) {
		int fd = dup2(teco_interface.stderr_orig, 2);
		g_assert(fd == 2);
	}
#endif

	/*
	 * cmdline_window determines whether we're in batch mode.
	 */
	if (teco_interface.cmdline_window) {
		delwin(teco_interface.cmdline_window);
		teco_interface.cmdline_window = NULL;
	}
}

static void
teco_interface_resize_all_windows(void)
{
	int lines, cols; /* screen dimensions */

	getmaxyx(stdscr, lines, cols);

	wresize(teco_interface.info_window, 1, cols);
	wresize(teco_view_get_window(teco_interface_current_view),
	        lines - 3, cols);
	wresize(teco_interface.msg_window, 1, cols);
	mvwin(teco_interface.msg_window, lines - 2, 0);
	wresize(teco_interface.cmdline_window, 1, cols);
	mvwin(teco_interface.cmdline_window, lines - 1, 0);

	teco_interface_draw_info();
	teco_interface_msg_clear(); /* FIXME: use saved message */
	teco_interface_popup_clear();
	teco_interface_draw_cmdline();
}

void
teco_interface_vmsg(teco_msg_t type, const gchar *fmt, va_list ap)
{
	if (!teco_interface.cmdline_window) { /* batch mode */
		teco_interface_stdio_vmsg(type, fmt, ap);
		return;
	}

	/*
	 * On most platforms we can write to stdout/stderr
	 * even in interactive mode.
	 */
#if defined(PDCURSES_GUI) || defined(CURSES_TTY) || defined(NCURSES_WIN32)
	va_list aq;
	va_copy(aq, ap);
	teco_interface_stdio_vmsg(type, fmt, aq);
	va_end(aq);
#endif

	short fg, bg;

	fg = teco_rgb2curses(teco_interface_ssm(SCI_STYLEGETBACK, STYLE_DEFAULT, 0));

	switch (type) {
	default:
	case TECO_MSG_USER:
		bg = teco_rgb2curses(teco_interface_ssm(SCI_STYLEGETFORE, STYLE_DEFAULT, 0));
		break;
	case TECO_MSG_INFO:
		bg = COLOR_GREEN;
		break;
	case TECO_MSG_WARNING:
		bg = COLOR_YELLOW;
		break;
	case TECO_MSG_ERROR:
		bg = COLOR_RED;
		beep();
		break;
	}

	wmove(teco_interface.msg_window, 0, 0);
	wbkgdset(teco_interface.msg_window, ' ' | SCI_COLOR_ATTR(fg, bg));
	vw_printw(teco_interface.msg_window, fmt, ap);
	wclrtoeol(teco_interface.msg_window);
}

void
teco_interface_msg_clear(void)
{
	if (!teco_interface.cmdline_window) /* batch mode */
		return;

	short fg = teco_rgb2curses(teco_interface_ssm(SCI_STYLEGETBACK, STYLE_DEFAULT, 0));
	short bg = teco_rgb2curses(teco_interface_ssm(SCI_STYLEGETFORE, STYLE_DEFAULT, 0));

	wbkgdset(teco_interface.msg_window, ' ' | SCI_COLOR_ATTR(fg, bg));
	werase(teco_interface.msg_window);
}

void
teco_interface_show_view(teco_view_t *view)
{
	teco_interface_current_view = view;

	if (!teco_interface.cmdline_window) /* batch mode */
		return;

	WINDOW *current_view_win = teco_view_get_window(teco_interface_current_view);

	/*
	 * screen size might have changed since
	 * this view's WINDOW was last active
	 */
	int lines, cols; /* screen dimensions */
	getmaxyx(stdscr, lines, cols);
	wresize(current_view_win, lines - 3, cols);
	/* Set up window position: never changes */
	mvwin(current_view_win, 1, 0);
}

#if PDCURSES

static void
teco_interface_set_window_title(const gchar *title)
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

#elif defined(CURSES_TTY) && defined(HAVE_TIGETSTR)

static void
teco_interface_set_window_title(const gchar *title)
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
	fputs(to_status_line, teco_interface.screen_tty);
	fputs(title, teco_interface.screen_tty);
	fputs(from_status_line, teco_interface.screen_tty);
	fflush(teco_interface.screen_tty);
}

#else /* !PDCURSES && (!CURSES_TTY || !HAVE_TIGETSTR) */

static void
teco_interface_set_window_title(const gchar *title)
{
	/* no way to set window title */
}

#endif

static void
teco_interface_draw_info(void)
{
	if (!teco_interface.info_window) /* batch mode */
		return;

	/*
	 * The info line is printed in reverse colors of
	 * the current buffer's STYLE_DEFAULT.
	 * The same style is used for MSG_USER messages.
	 */
	short fg = teco_rgb2curses(teco_interface_ssm(SCI_STYLEGETBACK, STYLE_DEFAULT, 0));
	short bg = teco_rgb2curses(teco_interface_ssm(SCI_STYLEGETFORE, STYLE_DEFAULT, 0));

	wmove(teco_interface.info_window, 0, 0);
	wbkgdset(teco_interface.info_window, ' ' | SCI_COLOR_ATTR(fg, bg));

	const gchar *info_type_str;

	switch (teco_interface.info_type) {
	case TECO_INFO_TYPE_QREG:
		info_type_str = PACKAGE_NAME " - <QRegister> ";
		waddstr(teco_interface.info_window, info_type_str);
		/* same formatting as in command lines */
		teco_curses_format_str(teco_interface.info_window,
		                       teco_interface.info_current.data,
		                       teco_interface.info_current.len, -1);
		break;

	case TECO_INFO_TYPE_BUFFER:
		info_type_str = PACKAGE_NAME " - <Buffer> ";
		waddstr(teco_interface.info_window, info_type_str);
		g_assert(!teco_string_contains(&teco_interface.info_current, '\0'));
		teco_curses_format_filename(teco_interface.info_window,
		                            teco_interface.info_current.data, -1);
		break;

	default:
		g_assert_not_reached();
	}

	wclrtoeol(teco_interface.info_window);

	/*
	 * Make sure the title will consist only of printable
	 * characters
	 */
	g_autofree gchar *info_current_printable;
	info_current_printable = teco_string_echo(teco_interface.info_current.data,
	                                          teco_interface.info_current.len);
	g_autofree gchar *title = g_strconcat(info_type_str, info_current_printable, NULL);
	teco_interface_set_window_title(title);
}

void
teco_interface_info_update_qreg(const teco_qreg_t *reg)
{
	teco_string_clear(&teco_interface.info_current);
	teco_string_init(&teco_interface.info_current,
	                 reg->head.name.data, reg->head.name.len);
	teco_interface.info_type = TECO_INFO_TYPE_QREG;
	/* NOTE: drawn in teco_interface_event_loop_iter() */
}

void
teco_interface_info_update_buffer(const teco_buffer_t *buffer)
{
	const gchar *filename = buffer->filename ? : UNNAMED_FILE;

	teco_string_clear(&teco_interface.info_current);
	teco_string_init(&teco_interface.info_current, filename, strlen(filename));
	teco_string_append_c(&teco_interface.info_current,
	                     buffer->dirty ? '*' : ' ');
	teco_interface.info_type = TECO_INFO_TYPE_BUFFER;
	/* NOTE: drawn in teco_interface_event_loop_iter() */
}

void
teco_interface_cmdline_update(const teco_cmdline_t *cmdline)
{
	/*
	 * Replace entire pre-formatted command-line.
	 * We don't know if it is similar to the last one,
	 * so resizing makes no sense.
	 * We approximate the size of the new formatted command-line,
	 * wasting a few bytes for control characters.
	 */
	if (teco_interface.cmdline_pad)
		delwin(teco_interface.cmdline_pad);

	int max_cols = 1;
	for (guint i = 0; i < cmdline->str.len; i++)
		max_cols += TECO_IS_CTL(cmdline->str.data[i]) ? 3 : 1;
	teco_interface.cmdline_pad = newpad(1, max_cols);

	short fg = teco_rgb2curses(teco_interface_ssm(SCI_STYLEGETFORE, STYLE_DEFAULT, 0));
	short bg = teco_rgb2curses(teco_interface_ssm(SCI_STYLEGETBACK, STYLE_DEFAULT, 0));
	wcolor_set(teco_interface.cmdline_pad, SCI_COLOR_PAIR(fg, bg), NULL);

	/* format effective command line */
	teco_interface.cmdline_len =
		teco_curses_format_str(teco_interface.cmdline_pad,
		                       cmdline->str.data, cmdline->effective_len, -1);

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
	wattron(teco_interface.cmdline_pad, A_UNDERLINE | A_BOLD);

	/*
	 * Format rubbed-out command line.
	 * NOTE: This formatting will never be truncated since we're
	 * writing into the pad which is large enough.
	 */
	teco_interface.cmdline_rubout_len =
		teco_curses_format_str(teco_interface.cmdline_pad, cmdline->str.data + cmdline->effective_len,
		                       cmdline->str.len - cmdline->effective_len, -1);

	/*
	 * Highlight cursor after effective command line
	 * FIXME: This should use SCI_GETCARETFORE().
	 */
	if (teco_interface.cmdline_rubout_len) {
		attr_t attr = 0;
		short pair = 0;

		wmove(teco_interface.cmdline_pad, 0, teco_interface.cmdline_len);
		wattr_get(teco_interface.cmdline_pad, &attr, &pair, NULL);
		wchgat(teco_interface.cmdline_pad, 1,
		       (attr & A_UNDERLINE) | A_REVERSE, pair, NULL);
	} else {
		teco_interface.cmdline_len++;
		wattroff(teco_interface.cmdline_pad, A_UNDERLINE | A_BOLD);
		waddch(teco_interface.cmdline_pad, ' ' | A_REVERSE);
	}

	teco_interface_draw_cmdline();
}

static void
teco_interface_draw_cmdline(void)
{
	/* total width available for command line */
	guint total_width = getmaxx(teco_interface.cmdline_window) - 1;

	/* beginning of command line to show */
	guint disp_offset = teco_interface.cmdline_len -
	                    MIN(teco_interface.cmdline_len,
	                        total_width/2 + teco_interface.cmdline_len % MAX(total_width/2, 1));
	/*
	 * length of command line to show
	 *
	 * NOTE: we do not use getmaxx(cmdline_pad) here since it may be
	 * larger than the text the pad contains.
	 */
	guint disp_len = MIN(total_width, teco_interface.cmdline_len +
	                     teco_interface.cmdline_rubout_len - disp_offset);

	short fg = teco_rgb2curses(teco_interface_ssm(SCI_STYLEGETFORE, STYLE_DEFAULT, 0));
	short bg = teco_rgb2curses(teco_interface_ssm(SCI_STYLEGETBACK, STYLE_DEFAULT, 0));

	wbkgdset(teco_interface.cmdline_window, ' ' | SCI_COLOR_ATTR(fg, bg));
	werase(teco_interface.cmdline_window);
	mvwaddch(teco_interface.cmdline_window, 0, 0, '*' | A_BOLD);
	copywin(teco_interface.cmdline_pad, teco_interface.cmdline_window,
	        0, disp_offset, 0, 1, 0, disp_len, FALSE);
}

#if PDCURSES

/*
 * At least on PDCurses, a single clipboard
 * can be supported. We register it as the
 * default clipboard ("~") as we do not know whether
 * it corresponds to the X11 PRIMARY, SECONDARY or
 * CLIPBOARD selections.
 */
static void
teco_interface_init_clipboard(void)
{
	char *contents;
	long length;

	/*
	 * Even on PDCurses, while the clipboard functions are
	 * available, the clipboard might not actually be supported.
	 * Since the existence of the QReg serves as an indication
	 * of clipboard support in SciTECO, we must first probe the
	 * usability of the clipboard.
	 * This could be done at compile time, but this way is more
	 * generic (albeit inefficient).
	 */
	int rc = PDC_getclipboard(&contents, &length);
	if (rc == PDC_CLIP_ACCESS_ERROR)
		return;
	if (rc == PDC_CLIP_SUCCESS)
		PDC_freeclipboard(contents);

	teco_qreg_table_insert(&teco_qreg_table_globals, teco_qreg_clipboard_new(""));
}

gboolean
teco_interface_set_clipboard(const gchar *name, const gchar *str, gsize str_len, GError **error)
{
	int rc = str ? PDC_setclipboard(str, str_len) : PDC_clearclipboard();
	if (rc != PDC_CLIP_SUCCESS) {
		g_set_error(error, TECO_ERROR, TECO_ERROR_FAILED,
		            "Error %d copying to clipboard", rc);
		return FALSE;
	}

	return TRUE;
}

gboolean
teco_interface_get_clipboard(const gchar *name, gchar **str, gsize *len, GError **error)
{
	char *contents;
	long length = 0;

	/*
	 * NOTE: It is undefined whether we can pass in NULL for length.
	 */
	int rc = PDC_getclipboard(&contents, &length);
	*len = length;
	if (rc == PDC_CLIP_EMPTY)
		return TRUE;
	if (rc != PDC_CLIP_SUCCESS) {
		g_set_error(error, TECO_ERROR, TECO_ERROR_FAILED,
		            "Error %d retrieving clipboard", rc);
		return FALSE;
	}

	/*
	 * PDCurses defines its own free function and there is no
	 * way to find out which allocator was used.
	 * We must therefore copy the memory to be on the safe side.
	 * At least, the result is guaranteed to be null-terminated
	 * and thus teco_string_t-compatible
	 * (PDCurses does not guarantee that either).
	 */
	if (str) {
		g_assert(contents != NULL);
		*str = memcpy(g_malloc(length + 1), contents, length);
		(*str)[length] = '\0';
	}

	PDC_freeclipboard(contents);
	return TRUE;
}

#elif defined(CURSES_TTY)

static void
teco_interface_init_clipboard(void)
{
	/*
	 * At least on XTerm, there are escape sequences
	 * for modifying the clipboard (OSC-52).
	 * This is not standardized in terminfo, so we add special
	 * XTerm support here. Unfortunately, it is pretty hard to find out
	 * whether clipboard operations will actually work.
	 * XTerm must be at least at v203 and the corresponding window operations
	 * must be enabled.
	 * There is no way to find out if they are but we must
	 * not register the clipboard registers if they aren't.
	 * Therefore, a special XTerm clipboard ED flag an be set by the user.
	 */
	if (!(teco_ed & TECO_ED_XTERM_CLIPBOARD) || teco_xterm_version() < 203)
		return;

	teco_qreg_table_insert(&teco_qreg_table_globals, teco_qreg_clipboard_new(""));
	teco_qreg_table_insert(&teco_qreg_table_globals, teco_qreg_clipboard_new("P"));
	teco_qreg_table_insert(&teco_qreg_table_globals, teco_qreg_clipboard_new("S"));
	teco_qreg_table_insert(&teco_qreg_table_globals, teco_qreg_clipboard_new("C"));
}

static inline gchar
get_selection_by_name(const gchar *name)
{
	/*
	 * Only the first letter of name is significant.
	 * We allow to address the XTerm cut buffers as well
	 * (everything gets passed down), but currently we
	 * only register the three standard registers
	 * "~", "~P", "~S" and "~C".
	 */
	return g_ascii_tolower(*name) ? : 'c';
}

gboolean
teco_interface_set_clipboard(const gchar *name, const gchar *str, gsize str_len,
                             GError **error)
{
	fputs("\e]52;", teco_interface.screen_tty);
	fputc(get_selection_by_name(name), teco_interface.screen_tty);
	fputc(';', teco_interface.screen_tty);

	/*
	 * Enough space for 1024 Base64-encoded bytes.
	 */
	gchar buffer[(1024 / 3) * 4 + 4];
	gsize out_len;
	/* g_base64_encode_step() state: */
	gint state = 0, save = 0;

	while (str_len > 0) {
		gsize step_len = MIN(1024, str_len);

		/*
		 * This could be simplified using g_base64_encode().
		 * However, doing it step-wise avoids an allocation.
		 */
		out_len = g_base64_encode_step((const guchar *)str,
		                               step_len, FALSE,
		                               buffer, &state, &save);
		fwrite(buffer, 1, out_len, teco_interface.screen_tty);

		str_len -= step_len;
		str += step_len;
	}

	out_len = g_base64_encode_close(FALSE, buffer, &state, &save);
	fwrite(buffer, 1, out_len, teco_interface.screen_tty);

	fputc('\a', teco_interface.screen_tty);
	fflush(teco_interface.screen_tty);

	return TRUE;
}

gboolean
teco_interface_get_clipboard(const gchar *name, gchar **str, gsize *len, GError **error)
{
	/*
	 * Query the clipboard -- XTerm will reply with the
	 * OSC-52 command that would set the current selection.
	 */
	fputs("\e]52;", teco_interface.screen_tty);
	fputc(get_selection_by_name(name), teco_interface.screen_tty);
	fputs(";?\a", teco_interface.screen_tty);
	fflush(teco_interface.screen_tty);

	/*
	 * It is very well possible that the XTerm clipboard
	 * is not working because it is disabled, so we
	 * must be prepared for timeouts when reading.
	 * That's why we're using the Curses API here, instead
	 * of accessing screen_tty directly. It gives us a relatively
	 * simple way to read with timeouts.
	 * We restore all changed Curses settings before returning
	 * to be on the safe side.
	 */
	halfdelay(1); /* 100ms timeout */
	keypad(stdscr, FALSE);

	/*
	 * Skip "\e]52;x;" (7 characters).
	 */
	for (gint i = 0; i < 7; i++) {
		if (getch() == ERR) {
			/* timeout */
			cbreak();
			g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
			                    "Timed out reading XTerm clipboard");
			return FALSE;
		}
	}

	GString *str_base64 = g_string_new("");
	/* g_base64_decode_step() state: */
	gint state = 0;
	guint save = 0;

	for (;;) {
		/*
		 * Space for storing one group of decoded Base64 characters
		 * and the OSC-52 response.
		 */
		gchar buffer[MAX(3, 7)];

		gchar c = (gchar)getch();
		if (c == ERR) {
			/* timeout */
			cbreak();
			g_string_free(str_base64, TRUE);
			g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
			                    "Timed out reading XTerm clipboard");
			return FALSE;
		}
		if (c == '\a')
			break;

		/*
		 * This could be simplified using sscanf() and
		 * g_base64_decode(), but we avoid one allocation
		 * to get the entire Base64 string.
		 * (Also to allow for timeouts, we should
		 * read character-wise using getch() anyway.)
		 */
		gsize out_len = g_base64_decode_step(&c, sizeof(c),
		                                     (guchar *)buffer,
		                                     &state, &save);
		g_string_append_len(str_base64, buffer, out_len);
	}

	cbreak();

	if (str)
		*str = str_base64->str;
	*len = str_base64->len;

	g_string_free(str_base64, !str);
	return TRUE;
}

#else /* !PDCURSES && !CURSES_TTY */

static void
teco_interface_init_clipboard(void)
{
	/*
	 * No native clipboard support, so no clipboard Q-Regs are
	 * registered.
	 */
}

gboolean
teco_interface_set_clipboard(const gchar *name, const gchar *str, gsize str_len,
                             GError **error)
{
	g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
	                    "Setting clipboard unsupported");
	return FALSE;
}

gboolean
teco_interface_get_clipboard(const gchar *name, gchar **str, gsize *len, GError **error)
{
	g_set_error_literal(error, TECO_ERROR, TECO_ERROR_FAILED,
	                    "Getting clipboard unsupported");
	return FALSE;
}

#endif

void
teco_interface_popup_add(teco_popup_entry_type_t type, const gchar *name, gsize name_len,
                         gboolean highlight)
{
	if (teco_interface.cmdline_window)
		/* interactive mode */
		teco_curses_info_popup_add(&teco_interface.popup, type, name, name_len, highlight);
}

void
teco_interface_popup_show(void)
{
	if (!teco_interface.cmdline_window)
		/* batch mode */
		return;

	short fg = teco_rgb2curses(teco_interface_ssm(SCI_STYLEGETFORE, STYLE_CALLTIP, 0));
	short bg = teco_rgb2curses(teco_interface_ssm(SCI_STYLEGETBACK, STYLE_CALLTIP, 0));

	teco_curses_info_popup_show(&teco_interface.popup, SCI_COLOR_ATTR(fg, bg));
}

gboolean
teco_interface_popup_is_shown(void)
{
	return teco_curses_info_popup_is_shown(&teco_interface.popup);
}

void
teco_interface_popup_clear(void)
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
	if (teco_curses_info_popup_is_shown(&teco_interface.popup)) {
		touchwin(teco_interface.info_window);
		touchwin(teco_interface.msg_window);
	}
#endif

	teco_curses_info_popup_clear(&teco_interface.popup);
	teco_curses_info_popup_init(&teco_interface.popup);
}

#if defined(CURSES_TTY) || defined(PDCURSES_WINCON) || defined(NCURSES_WIN32)

/*
 * For UNIX Curses we can rely on signal handlers to detect interruptions via CTRL+C.
 * On Win32 console builds, there is teco_console_ctrl_handler().
 */
gboolean
teco_interface_is_interrupted(void)
{
	return teco_interrupted != FALSE;
}

#else /* !CURSES_TTY && !PDCURSES_WINCON && !NCURSES_WIN32 */

/*
 * This function is called repeatedly, so we can poll the keyboard input queue,
 * filtering out CTRL+C.
 * It's currently necessary as a fallback e.g. for PDCURSES_GUI or XCurses.
 *
 * NOTE: Theoretically, this can be optimized by doing wgetch() only every X
 * microseconds like on Gtk+.
 * But this turned out to slow things down, at least on PDCurses/WinGUI.
 */
gboolean
teco_interface_is_interrupted(void)
{
	if (!teco_interface.cmdline_window)
		/* batch mode */
		return teco_interrupted != FALSE;

	/* NOTE: getch() is configured to be nonblocking. */
	gint key;
	while ((key = wgetch(teco_interface.cmdline_window)) != ERR) {
		if (G_UNLIKELY(key == TECO_CTL_KEY('C')))
			return TRUE;
		g_queue_push_tail(teco_interface.input_queue,
		                  GINT_TO_POINTER(key));
	}

	return teco_interrupted != FALSE;
}

#endif

static void
teco_interface_refresh(void)
{
	/*
	 * Scintilla has been patched to avoid any automatic scrolling since that
	 * has been benchmarked to be a very costly operation.
	 * Instead we do it only once after every keypress.
	 */
	teco_interface_ssm(SCI_SCROLLCARET, 0, 0);

	/*
	 * Info window is updated very often which is very
	 * costly, especially when using PDC_set_title(),
	 * so we redraw it here, where the overhead does
	 * not matter much.
	 */
	teco_interface_draw_info();
	wnoutrefresh(teco_interface.info_window);
	teco_view_noutrefresh(teco_interface_current_view);
	wnoutrefresh(teco_interface.msg_window);
	wnoutrefresh(teco_interface.cmdline_window);
	teco_curses_info_popup_noutrefresh(&teco_interface.popup);
	doupdate();
}

static gint
teco_interface_blocking_getch(void)
{
	/*
	 * Setting function key processing is important
	 * on Unix Curses, as ESCAPE is handled as the beginning
	 * of a escape sequence when terminal emulators are
	 * involved.
	 * On some Curses variants (XCurses) however, keypad
	 * must always be TRUE so we receive KEY_RESIZE.
	 *
	 * FIXME: NetBSD's curses could be handled like ncurses,
	 * but gets into an undefined state when SciTECO processes
	 * escape sequences.
	 */
#ifdef NCURSES_UNIX
	keypad(teco_interface.cmdline_window, teco_ed & TECO_ED_FNKEYS);
#endif

	/* no special <CTRL/C> handling */
	raw();
	nodelay(teco_interface.cmdline_window, FALSE);
	/*
	 * Memory limiting is stopped temporarily, since it might otherwise
	 * constantly place 100% load on the CPU.
	 */
	teco_memory_stop_limiting();
	gint key = wgetch(teco_interface.cmdline_window);
	teco_memory_start_limiting();
	/* allow asynchronous interruptions on <CTRL/C> */
	teco_interrupted = FALSE;
	nodelay(teco_interface.cmdline_window, TRUE);
#if defined(CURSES_TTY) || defined(PDCURSES_WINCON) || defined(NCURSES_WIN32)
	noraw(); /* FIXME: necessary because of NCURSES_WIN32 bug */
	cbreak();
#endif

	return key;
}

/**
 * One iteration of the event loop.
 *
 * This is a global function, so it may be used as an asynchronous Emscripten callback.
 * While this function cannot directly throw GErrors,
 * it can set teco_interface.event_loop_error.
 *
 * @fixme Thrown errors should be somehow caught when building for EMScripten as well.
 * Perhaps in a goto-block.
 */
void
teco_interface_event_loop_iter(void)
{
	gint key = g_queue_is_empty(teco_interface.input_queue)
			? teco_interface_blocking_getch()
			: GPOINTER_TO_INT(g_queue_pop_head(teco_interface.input_queue));

	switch (key) {
	case ERR:
		/* shouldn't really happen */
		return;
#ifdef KEY_RESIZE
	case KEY_RESIZE:
#ifdef __PDCURSES__
		/* NOTE: No longer necessary since PDCursesMod v4.3.3. */
		resize_term(0, 0);
#endif
		teco_interface_resize_all_windows();
		break;
#endif
	case TECO_CTL_KEY('H'):
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
		if (!teco_cmdline_keypress_c(TECO_CTL_KEY('H'),
		                             &teco_interface.event_loop_error))
			return;
		break;
	case KEY_ENTER:
	case '\r':
	case '\n':
		if (!teco_cmdline_keypress_c('\n', &teco_interface.event_loop_error))
			return;
		break;

	/*
	 * Function key macros
	 */
#define FN(KEY) \
	case KEY_##KEY: \
		if (!teco_cmdline_fnmacro(#KEY, &teco_interface.event_loop_error)) \
			return; \
		break
#define FNS(KEY) FN(KEY); FN(S##KEY)
	FN(DOWN); FN(UP); FNS(LEFT); FNS(RIGHT);
	FNS(HOME);
	case KEY_F(0)...KEY_F(63): {
		gchar macro_name[3+1];

		g_snprintf(macro_name, sizeof(macro_name),
			   "F%d", key - KEY_F0);
		if (!teco_cmdline_fnmacro(macro_name,
		                          &teco_interface.event_loop_error))
			return;
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
		if (key < 0x80 &&
		    !teco_cmdline_keypress_c(key, &teco_interface.event_loop_error))
			return;
	}

	teco_interface_refresh();
}

gboolean
teco_interface_event_loop(GError **error)
{
	/*
	 * Initialize Curses for interactive mode
	 */
	if (!teco_interface_init_interactive(error))
		return FALSE;

	static const teco_cmdline_t empty_cmdline; // FIXME
	teco_interface_cmdline_update(&empty_cmdline);
	teco_interface_msg_clear();
	teco_interface_refresh();

#ifdef EMCURSES
	PDC_emscripten_set_handler(teco_interface_event_loop_iter, TRUE);
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
	while (!teco_interface.event_loop_error)
		teco_interface_event_loop_iter();

	/*
	 * The error needs to be propagated only if this is
	 * NOT a SciTECO termination (e.g. EX$$)
	 */
	if (!g_error_matches(teco_interface.event_loop_error,
	                     TECO_ERROR, TECO_ERROR_QUIT)) {
		g_propagate_error(error, g_steal_pointer(&teco_interface.event_loop_error));
		return FALSE;
	}
	g_clear_error(&teco_interface.event_loop_error);

	teco_interface_restore_batch();
#endif

	return TRUE;
}

void
teco_interface_cleanup(void)
{
	if (teco_interface.event_loop_error)
		g_error_free(teco_interface.event_loop_error);

	if (teco_interface.info_window)
		delwin(teco_interface.info_window);
	teco_string_clear(&teco_interface.info_current);
	if (teco_interface.input_queue)
		g_queue_free(teco_interface.input_queue);
	if (teco_interface.cmdline_window)
		delwin(teco_interface.cmdline_window);
	if (teco_interface.cmdline_pad)
		delwin(teco_interface.cmdline_pad);
	if (teco_interface.msg_window)
		delwin(teco_interface.msg_window);

	/*
	 * PDCurses/WinCon crashes if initscr() wasn't called.
	 * Others (XCurses) crash if we try to use isendwin() here.
	 * Perhaps Curses cleanup should be in restore_batch()
	 * instead.
	 */
#ifndef XCURSES
	if (teco_interface.info_window && !isendwin())
		endwin();
#endif

	if (teco_interface.screen)
		delscreen(teco_interface.screen);
	if (teco_interface.screen_tty)
		fclose(teco_interface.screen_tty);
	if (teco_interface.stderr_orig >= 0)
		close(teco_interface.stderr_orig);
	if (teco_interface.stdout_orig >= 0)
		close(teco_interface.stdout_orig);
}
