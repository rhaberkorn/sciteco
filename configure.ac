#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.65])
AC_INIT([SciTECO], [0.1],
	[robin.haberkorn@googlemail.com],
	[sciteco],
	[https://github.com/rhaberkorn/sciteco])
AC_CONFIG_AUX_DIR(config)
AM_INIT_AUTOMAKE

AC_CONFIG_SRCDIR([src/main.cpp])
AC_CONFIG_HEADERS([config.h])

AC_CANONICAL_BUILD
AC_CANONICAL_HOST

# Data directory
sciteco_datadir=$datarootdir/$PACKAGE_TARNAME
AC_SUBST(sciteco_datadir)

# Checks for programs.
LT_INIT
AC_PROG_CXX
AC_PROG_CC
AC_PROG_CC_C99
AC_PROG_INSTALL

AC_CHECK_PROG(READLINK, readlink, readlink)
if [[ x$READLINK = x ]]; then
	AC_MSG_ERROR([Required tool readlink not found!])
fi

# Checks for libraries.
PKG_CHECK_MODULES(LIBGLIB, [glib-2.0], [
	CFLAGS="$CFLAGS $LIBGLIB_CFLAGS"
	LDFLAGS="$LDFLAGS $LIBGLIB_LIBS"
])

# Checks for header files.
AC_HEADER_STDC

AC_CHECK_HEADERS([bsd/sys/queue.h])
AM_CONDITIONAL(NEED_COMPAT, [test $ac_cv_header_bsd_sys_queue_h = no])

case $host in
*-*-mingw*)
	AC_CHECK_HEADERS([windows.h], , [
		AC_MSG_ERROR([Missing Windows headers!])
	])
	;;
esac

# Checks for typedefs, structures, and compiler characteristics.
AC_HEADER_STDBOOL
AC_C_INLINE
AC_TYPE_SIZE_T

# Checks for library functions.
AC_CHECK_FUNCS([memset setlocale strchr strrchr])

case $host in
*-*-linux*)
	AC_CHECK_FUNC([realpath])
	;;
esac

#
# Config options
#
AC_ARG_WITH(scintilla,
	AS_HELP_STRING([--with-scintilla=PATH],
		       [Specify Scintilla's path [default=..]]),
	[SCINTILLA_PATH=$withval], [SCINTILLA_PATH=..])
SCINTILLA_PATH=`$READLINK -e $SCINTILLA_PATH`
AC_SUBST(SCINTILLA_PATH)

CFLAGS="$CFLAGS -I$SCINTILLA_PATH/include -DSCI_LEXER"
LDFLAGS="$LDFLAGS $SCINTILLA_PATH/bin/scintilla.a"

AC_ARG_WITH(interface,
	AS_HELP_STRING([--with-interface=ncurses|pdcurses|gtk],
		       [Specify user interface [default=ncurses]]),
	[INTERFACE=$withval], [INTERFACE=ncurses])

case $INTERFACE in
ncurses)
	AC_CHECK_LIB(ncurses, initscr, , [
		AC_MSG_ERROR([libncurses missing!])
	])
	AC_CHECK_HEADERS([curses.h], , [
		AC_MSG_ERROR([Curses header missing!])
	])
	AC_DEFINE(INTERFACE_NCURSES, , [Build with curses support])

	# For Scintilla:
	CFLAGS="$CFLAGS -DNCURSES -I$SCINTILLA_PATH/scinterm"
	;;

pdcurses)
	AC_CHECK_LIB(pdcurses, initscr, , [
		AC_MSG_ERROR([libpdcurses missing!])
	])
	AC_CHECK_HEADERS([curses.h], , [
		AC_MSG_ERROR([Curses header missing!])
	])
	AC_DEFINE(INTERFACE_NCURSES, , [Build with curses support])

	AC_CHECK_FUNC([PDC_set_resize_limits], [
		AC_DEFINE(PDCURSES_WIN32A, , [PDCurses supports Win32a extensions])
	])

	# For Scintilla:
	CFLAGS="$CFLAGS -DNCURSES -I$SCINTILLA_PATH/scinterm"
	;;

gtk)
	PKG_CHECK_MODULES(LIBGTK, [gtk+-2.0], [
		CFLAGS="$CFLAGS $LIBGTK_CFLAGS"
		LDFLAGS="$LDFLAGS $LIBGTK_LIBS"
	])
	AC_CHECK_PROG(GOB2, gob2, gob2)
	if [[ x$GOB2 = x ]]; then
		AC_MSG_ERROR([Gob2 (GObject Builder) not found!])
	fi
	AC_DEFINE(INTERFACE_GTK, , [Build with GTK+ 2.0 support])

	# For Scintilla:
	CFLAGS="$CFLAGS -DGTK"
	;;

*)
	AC_MSG_ERROR([Invalid user interface specified!])
	;;
esac

AM_CONDITIONAL(INTERFACE_GTK, [test x$INTERFACE = xgtk])

CXXFLAGS="$CXXFLAGS $CFLAGS"

AC_CONFIG_FILES([Makefile src/Makefile])
AC_OUTPUT
