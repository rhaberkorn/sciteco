# SYNOPSIS
#
#   AX_WITH_NCURSES
#
# DESCRIPTION
#
#   This macro checks for an ncurses library with enhanced definitions
#   providing a curses.h either in the default search path or as
#   established by pkg-config.
#
#   It is based on the AX_WITH_CURSES macro but does not attempt
#   to find any non-standard header, which would require #ifdefing
#   over all the possible headers or using a computed #include.
#   This would complicate ncurses detection unnecessarily especially
#   since Scinterm needs to include a curses header as well.
#   ncurses packages, which do not ship a pkg-config script and
#   do not install curses.h into a standard include path
#   (e.g. NetBSD's pkgsrc ncurses package) will *not* be detected
#   by this macro unless defining CURSES_CFLAGS.
#
#   The following preprocessor symbols may be defined by this macro if the
#   appropriate conditions are met:
#
#     HAVE_CURSES             - if any SysV or X/Open Curses library found
#     HAVE_CURSES_ENHANCED    - if library supports X/Open Enhanced functions
#     HAVE_CURSES_COLOR       - if library supports color (enhanced functions)
#     HAVE_CURSES_OBSOLETE    - if library supports certain obsolete features
#     HAVE_NCURSESW           - if NcursesW (wide char) library is to be used
#     HAVE_NCURSES            - if the Ncurses library is to be used
#
#     HAVE_CURSES_H           - if <curses.h> is present and should be used
#
#   (These preprocessor symbols are discussed later in this document.)
#
#   The following output variables are defined by this macro; they are
#   precious and may be overridden on the ./configure command line:
#
#     CURSES_LIBS  - library to add to xxx_LDADD
#     CURSES_CFLAGS  - include paths to add to xxx_CPPFLAGS
#
#   If CURSES_LIBS is set on the configure command line (such as by running
#   "./configure CURSES_LIBS=-lmycurses"), then the only header searched for
#   is <curses.h>. If the user needs to specify an alternative path for a
#   library (such as for a non-standard NcurseW), the user should use the
#   LDFLAGS variable.
#
#   The following shell variables may be defined by this macro:
#
#     ax_cv_curses           - set to "yes" if any Curses library found
#     ax_cv_curses_enhanced  - set to "yes" if Enhanced functions present
#     ax_cv_curses_color     - set to "yes" if color functions present
#     ax_cv_curses_obsolete  - set to "yes" if obsolete features present
#
#     ax_cv_ncursesw      - set to "yes" if NcursesW library found
#     ax_cv_ncurses       - set to "yes" if Ncurses library found
#     ax_cv_curses_which  - set to "ncursesw", "ncurses", "plaincurses" or "no"
#
#   These variables can be used in your configure.ac to determine the level
#   of support you need from the Curses library.  For example, if you must
#   have either Ncurses or NcursesW, you could include:
#
#     AX_WITH_NCURSES
#     if test "x$ax_cv_ncursesw" != xyes && test "x$ax_cv_ncurses" != xyes; then
#         AC_MSG_ERROR([requires either NcursesW or Ncurses library])
#     fi
#
#   HAVE_CURSES_OBSOLETE and ax_cv_curses_obsolete are defined if the
#   library supports certain features present in SysV and BSD Curses but not
#   defined in the X/Open definition.  In particular, the functions
#   getattrs(), getcurx() and getmaxx() are checked.
#
# LICENSE
#
#   Copyright (c) 2009 Mark Pulford <mark@kyne.com.au>
#   Copyright (c) 2009 Damian Pietras <daper@daper.net>
#   Copyright (c) 2012 Reuben Thomas <rrt@sc3d.org>
#   Copyright (c) 2011 John Zaitseff <J.Zaitseff@zap.org.au>
#   Copyright (c) 2025 Robin Haberkorn <robin.haberkorn@googlemail.com>
#
#   This program is free software: you can redistribute it and/or modify it
#   under the terms of the GNU General Public License as published by the
#   Free Software Foundation, either version 3 of the License, or (at your
#   option) any later version.
#
#   This program is distributed in the hope that it will be useful, but
#   WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
#   Public License for more details.
#
#   You should have received a copy of the GNU General Public License along
#   with this program. If not, see <https://www.gnu.org/licenses/>.
#
#   As a special exception, the respective Autoconf Macro's copyright owner
#   gives unlimited permission to copy, distribute and modify the configure
#   scripts that are the output of Autoconf when processing the Macro. You
#   need not follow the terms of the GNU General Public License when using
#   or distributing such scripts, even though portions of the text of the
#   Macro appear in them. The GNU General Public License (GPL) does govern
#   all other use of the material that constitutes the Autoconf Macro.
#
#   This special exception to the GPL applies to versions of the Autoconf
#   Macro released by the Autoconf Archive. When you make and distribute a
#   modified version of the Autoconf Macro, you may extend this special
#   exception to the GPL to apply to your modified version as well.

#serial 18

# internal function to factorize common code that is used by both ncurses
# and ncursesw
AC_DEFUN([_FIND_CURSES_FLAGS], [
    AC_MSG_CHECKING([for $1 via pkg-config])

    AX_REQUIRE_DEFINED([PKG_CHECK_EXISTS])
    pkg_failed=no
    _PKG_CONFIG([_ax_cv_$1_libs], [libs], [$1])
    _PKG_CONFIG([_ax_cv_$1_cppflags], [cflags], [$1])

    AS_IF([test "x$pkg_failed" = "xyes" || test "x$pkg_failed" = "xuntried"],[
        AC_MSG_RESULT([no])
        # No suitable .pc file found, have to find flags via fallback
        AC_CACHE_CHECK([for $1 via fallback], [ax_cv_$1], [
            AS_ECHO()
            pkg_cv__ax_cv_$1_libs="-l$1"
            pkg_cv__ax_cv_$1_cppflags="-D_GNU_SOURCE $CURSES_CFLAGS"
            LIBS="$ax_saved_LIBS $pkg_cv__ax_cv_$1_libs"
            CPPFLAGS="$ax_saved_CPPFLAGS $pkg_cv__ax_cv_$1_cppflags"

            AC_MSG_CHECKING([for initscr() with $pkg_cv__ax_cv_$1_libs])
            AC_LINK_IFELSE([AC_LANG_CALL([], [initscr])],
                [
                    AC_MSG_RESULT([yes])
                    AC_MSG_CHECKING([for nodelay() with $pkg_cv__ax_cv_$1_libs])
                    AC_LINK_IFELSE([AC_LANG_CALL([], [nodelay])],[
                        ax_cv_$1=yes
                        ],[
                        AC_MSG_RESULT([no])
                        m4_if(
                            [$1],[ncursesw],[pkg_cv__ax_cv_$1_libs="$pkg_cv__ax_cv_$1_libs -ltinfow"],
                            [$1],[ncurses],[pkg_cv__ax_cv_$1_libs="$pkg_cv__ax_cv_$1_libs -ltinfo"]
                        )
                        LIBS="$ax_saved_LIBS $pkg_cv__ax_cv_$1_libs"

                        AC_MSG_CHECKING([for nodelay() with $pkg_cv__ax_cv_$1_libs])
                        AC_LINK_IFELSE([AC_LANG_CALL([], [nodelay])],[
                            ax_cv_$1=yes
                            ],[
                            ax_cv_$1=no
                        ])
                    ])
                ],[
                    ax_cv_$1=no
            ])
        ])
        ],[
        AC_MSG_RESULT([yes])
        # Found .pc file, using its information
        LIBS="$ax_saved_LIBS $pkg_cv__ax_cv_$1_libs"
        CPPFLAGS="$ax_saved_CPPFLAGS $pkg_cv__ax_cv_$1_cppflags"
        ax_cv_$1=yes
    ])
])

AU_ALIAS([MP_WITH_NCURSES], [AX_WITH_NCURSES])
AC_DEFUN([AX_WITH_NCURSES], [
    AC_ARG_VAR([CURSES_LIBS], [linker library for Curses, e.g. -lcurses])
    AC_ARG_VAR([CURSES_CFLAGS], [preprocessor flags for Curses, e.g. -I/usr/include/ncursesw])

    ax_saved_LIBS=$LIBS
    ax_saved_CPPFLAGS=$CPPFLAGS

    # Test for NcursesW
    AS_IF([test "x$CURSES_LIBS" = x], [
        _FIND_CURSES_FLAGS([ncursesw])

        AS_IF([test "x$ax_cv_ncursesw" = xyes], [
            ax_cv_curses=yes
            ax_cv_curses_which=ncursesw
            CURSES_LIBS="$pkg_cv__ax_cv_ncursesw_libs"
            CURSES_CFLAGS="$pkg_cv__ax_cv_ncursesw_cppflags"
            AC_DEFINE([HAVE_NCURSESW], [1], [Define to 1 if the NcursesW library is present])
            AC_DEFINE([HAVE_CURSES],   [1], [Define to 1 if a SysV or X/Open compatible Curses library is present])

            AC_CACHE_CHECK([for working curses.h], [ax_cv_header_curses_h], [
                AC_LINK_IFELSE([AC_LANG_PROGRAM([[
                        @%:@define _XOPEN_SOURCE_EXTENDED 1
                        @%:@include <curses.h>
                        @%:@ifndef _XOPEN_CURSES
                        @%:@error "this Curses library is not enhanced"
                        "this Curses library is not enhanced"
                        @%:@endif
                    ]], [[
                        chtype a = A_BOLD;
                        int b = KEY_LEFT;
                        chtype c = COLOR_PAIR(1) & A_COLOR;
                        attr_t d = WA_NORMAL;
                        cchar_t e;
                        wint_t f;
                        int g = getattrs(stdscr);
                        int h = getcurx(stdscr) + getmaxx(stdscr);
                        initscr();
                        init_pair(1, COLOR_WHITE, COLOR_RED);
                        wattr_set(stdscr, d, 0, NULL);
                        wget_wch(stdscr, &f);
                    ]])],
                    [ax_cv_header_curses_h=yes],
                    [ax_cv_header_curses_h=no])
            ])
            AS_IF([test "x$ax_cv_header_curses_h" = xyes], [
                ax_cv_curses_enhanced=yes
                ax_cv_curses_color=yes
                ax_cv_curses_obsolete=yes
                AC_DEFINE([HAVE_CURSES_ENHANCED],   [1], [Define to 1 if library supports X/Open Enhanced functions])
                AC_DEFINE([HAVE_CURSES_COLOR],      [1], [Define to 1 if library supports color (enhanced functions)])
                AC_DEFINE([HAVE_CURSES_OBSOLETE],   [1], [Define to 1 if library supports certain obsolete features])
                AC_DEFINE([HAVE_CURSES_H],          [1], [Define to 1 if <curses.h> is present])
            ], [
                AC_MSG_WARN([could not find a working curses.h])
            ])
        ])
    ])
    unset pkg_cv__ax_cv_ncursesw_libs
    unset pkg_cv__ax_cv_ncursesw_cppflags

    # Test for Ncurses
    AS_IF([test "x$CURSES_LIBS" = x], [
        _FIND_CURSES_FLAGS([ncurses])

        AS_IF([test "x$ax_cv_ncurses" = xyes], [
            ax_cv_curses=yes
            ax_cv_curses_which=ncurses
            CURSES_LIBS="$pkg_cv__ax_cv_ncurses_libs"
            CURSES_CFLAGS="$pkg_cv__ax_cv_ncurses_cppflags"
            AC_DEFINE([HAVE_NCURSES], [1], [Define to 1 if the Ncurses library is present])
            AC_DEFINE([HAVE_CURSES],  [1], [Define to 1 if a SysV or X/Open compatible Curses library is present])

            AC_CACHE_CHECK([for working curses.h], [ax_cv_header_curses_h], [
                AC_LINK_IFELSE([AC_LANG_PROGRAM([[
                        @%:@define _XOPEN_SOURCE_EXTENDED 1
                        @%:@include <curses.h>
                        @%:@ifndef _XOPEN_CURSES
                        @%:@error "this Curses library is not enhanced"
                        "this Curses library is not enhanced"
                        @%:@endif
                    ]], [[
                        chtype a = A_BOLD;
                        int b = KEY_LEFT;
                        chtype c = COLOR_PAIR(1) & A_COLOR;
                        attr_t d = WA_NORMAL;
                        cchar_t e;
                        wint_t f;
                        int g = getattrs(stdscr);
                        int h = getcurx(stdscr) + getmaxx(stdscr);
                        initscr();
                        init_pair(1, COLOR_WHITE, COLOR_RED);
                        wattr_set(stdscr, d, 0, NULL);
                        wget_wch(stdscr, &f);
                    ]])],
                    [ax_cv_header_curses_h=yes],
                    [ax_cv_header_curses_h=no])
            ])
            AS_IF([test "x$ax_cv_header_curses_h" = xyes], [
                ax_cv_curses_enhanced=yes
                ax_cv_curses_color=yes
                ax_cv_curses_obsolete=yes
                AC_DEFINE([HAVE_CURSES_ENHANCED],   [1], [Define to 1 if library supports X/Open Enhanced functions])
                AC_DEFINE([HAVE_CURSES_COLOR],      [1], [Define to 1 if library supports color (enhanced functions)])
                AC_DEFINE([HAVE_CURSES_OBSOLETE],   [1], [Define to 1 if library supports certain obsolete features])
                AC_DEFINE([HAVE_CURSES_H],          [1], [Define to 1 if <curses.h> is present])
            ], [
                AC_MSG_WARN([could not find a working curses.h])
            ])
        ])
    ])
    unset pkg_cv__ax_cv_ncurses_libs
    unset pkg_cv__ax_cv_ncurses_cppflags

    AS_IF([test "x$ax_cv_curses"          != xyes], [ax_cv_curses=no])
    AS_IF([test "x$ax_cv_curses_enhanced" != xyes], [ax_cv_curses_enhanced=no])
    AS_IF([test "x$ax_cv_curses_color"    != xyes], [ax_cv_curses_color=no])
    AS_IF([test "x$ax_cv_curses_obsolete" != xyes], [ax_cv_curses_obsolete=no])

    LIBS=$ax_saved_LIBS
    CPPFLAGS=$ax_saved_CPPFLAGS

    unset ax_saved_LIBS
    unset ax_saved_CPPFLAGS
])dnl
