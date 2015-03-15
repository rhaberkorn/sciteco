#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.65])
AC_INIT([SciTECO], [0.6],
	[robin.haberkorn@googlemail.com],
	[sciteco],
	[http://sciteco.sf.net/])
PACKAGE_URL_DEV=https://github.com/rhaberkorn/sciteco
AC_SUBST(PACKAGE_URL_DEV)
AC_DEFINE_UNQUOTED(PACKAGE_URL_DEV, ["$PACKAGE_URL_DEV"],
                   [Package development homepage.])
AC_CONFIG_AUX_DIR(config)
AM_INIT_AUTOMAKE

AC_CONFIG_SRCDIR([src/main.cpp])
AC_CONFIG_HEADERS([config.h])

AC_CANONICAL_BUILD
AC_CANONICAL_HOST

# SciTECO library macro directory
scitecolibdir=$datadir/$PACKAGE/lib
AC_SUBST(scitecolibdir)

# Auxiliary functions

# expand $1 and print its absolute path
# similar to GNU readlink -e but portable
canonicalize() {
	(eval cd $1 2>/dev/null && pwd -P)
}

# Checks for programs.
LT_INIT
AC_PROG_CXX
AC_PROG_CC
AC_PROG_CC_C99
AC_PROG_GREP
AC_PROG_INSTALL

# Required by the Scintilla build process:
AC_CHECK_TOOL(AR, ar)

# Check for Windows resource compiler and define
# WIN32 conditional
case $host in
*-mingw*)
	LT_LANG([Windows Resource])
	AM_CONDITIONAL(WIN32, [true])
	;;
*)
	AM_CONDITIONAL(WIN32, [false])
	;;
esac

# Emscripten uses Clang as well
AM_CONDITIONAL(CLANG, [$CXX --version | $GREP -E "clang|Emscripten" >/dev/null])

AC_CHECK_PROG(DATE, date, date)
if [[ x$DATE = x ]]; then
	AC_MSG_ERROR([Required tool date not found!])
fi

# not necessarily required (depends on --enable-html-manual)
AC_CHECK_PROG(GROFF, groff, groff)

# not necessarily required (depends on --enable-developer-doc)
AC_CHECK_PROG(DOXYGEN, doxygen, doxygen)
AC_CHECK_PROG(DOT, dot, dot)
DOXYGEN_HAVE_DOT=YES
if [[ x$DOT = x ]]; then DOXYGEN_HAVE_DOT=NO; fi
AC_SUBST(DOXYGEN_HAVE_DOT)

# For bootstrapping via installed sciteco
AC_CHECK_PROG(SCITECO, sciteco, sciteco)

# Checks for libraries.
PKG_CHECK_MODULES(LIBGLIB, [glib-2.0], [
	CFLAGS="$CFLAGS $LIBGLIB_CFLAGS"
	CXXFLAGS="$CXXFLAGS $LIBGLIB_CFLAGS"
	LIBS="$LIBS $LIBGLIB_LIBS"
])

# Checks for header files.
AC_HEADER_STDC

AC_CHECK_HEADERS([bsd/sys/queue.h])
AM_CONDITIONAL(NEED_COMPAT, [test $ac_cv_header_bsd_sys_queue_h = no])

case $host in
*-mingw*)
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

# Check for optional GNU libc features.
# Will probably only be found on Linux.
AC_CHECK_HEADERS([malloc.h])
AC_CHECK_FUNCS([malloc_trim])

#
# Config options
#
AC_ARG_WITH(scintilla,
	AS_HELP_STRING([--with-scintilla=PATH],
		       [Specify Scintilla's path [default=./scintilla]]),
	[SCINTILLA_PATH=$withval], [SCINTILLA_PATH=./scintilla])
SCINTILLA_PATH=`canonicalize $SCINTILLA_PATH`
AC_SUBST(SCINTILLA_PATH)

CPPFLAGS="$CPPFLAGS -I$SCINTILLA_PATH/include -DSCI_LEXER"

AC_ARG_WITH(scinterm,
	AS_HELP_STRING([--with-scinterm=PATH],
		       [Specify Scinterm's path [default=SCINTILLA_PATH/scinterm]]),
	[SCINTERM_PATH=$withval], [SCINTERM_PATH=$SCINTILLA_PATH/scinterm])
SCINTERM_PATH=`canonicalize $SCINTERM_PATH`
AC_SUBST(SCINTERM_PATH)

AC_ARG_WITH(interface,
	AS_HELP_STRING([--with-interface=ncurses|pdcurses|emcurses|gtk],
		       [Specify user interface [default=ncurses]]),
	[INTERFACE=$withval], [INTERFACE=ncurses])

case $INTERFACE in
*curses)
	case $INTERFACE in
	ncurses)
		AC_CHECK_LIB(ncurses, initscr, , [
			AC_MSG_ERROR([libncurses missing!])
		])
		AC_CHECK_FUNCS([tigetstr])
		;;
	pdcurses)
		AC_CHECK_LIB(pdcurses, initscr, , [
			AC_MSG_ERROR([libpdcurses missing!
			              Perhaps it is not named correctly or has wrong permissions.])
		])
		AC_CHECK_FUNC([PDC_set_resize_limits], [
			AC_DEFINE(PDCURSES_WIN32A, , [PDCurses supports Win32a extensions])
		])
		;;
	emcurses)
		# EMCurses is the PDCurses/Emscripten port
		# However, ordinary library checks do not work
		# under Emscripten
		LIBS="$LIBS -lpdcurses"
		;;
	esac

	AC_CHECK_HEADERS([curses.h], , [
		AC_MSG_ERROR([Curses header missing!])
	])
	AC_DEFINE(INTERFACE_CURSES, , [Build with curses support])

	# For Scintilla:
	CPPFLAGS="$CPPFLAGS -DCURSES -I$SCINTERM_PATH"
	;;

gtk)
	PKG_CHECK_MODULES(LIBGTK, [gtk+-2.0 gmodule-2.0], [
		CFLAGS="$CFLAGS $LIBGTK_CFLAGS"
		CXXFLAGS="$CXXFLAGS $LIBGTK_CFLAGS"
		LIBS="$LIBS $LIBGTK_LIBS"
	])
	AC_CHECK_PROG(GOB2, gob2, gob2)
	if [[ x$GOB2 = x ]]; then
		AC_MSG_ERROR([Gob2 (GObject Builder) not found!])
	fi
	AC_DEFINE(INTERFACE_GTK, , [Build with GTK+ 2.0 support])

	# For Scintilla:
	CPPFLAGS="$CPPFLAGS -DGTK"
	;;

*)
	AC_MSG_ERROR([Invalid user interface specified!])
	;;
esac

AM_CONDITIONAL(INTERFACE_GTK, [test x$INTERFACE = xgtk])

AC_ARG_WITH(teco-integer,
	AS_HELP_STRING([--with-teco-integer=SIZE],
		       [Storage size of TECO integers in bits [default=64]]),
	[TECO_INTEGER=$withval], [TECO_INTEGER=64])
AC_SUBST(TECO_INTEGER)
AC_DEFINE_UNQUOTED(TECO_INTEGER, $TECO_INTEGER, [Storage size of TECO integers])

AC_ARG_ENABLE(html-manual,
	AS_HELP_STRING([--enable-html-manual],
		       [Generate and install HTML manuals using Groff [default=no]]),
	[html_man=$enableval], [html_man=no])
if [[ $html_man = yes -a x$GROFF = x ]]; then
        AC_MSG_ERROR([Enabled generating HTML manuals, but Groff cannot be found!
		      Try --disable-html-manual.])
fi
AM_CONDITIONAL(BUILD_HTMLMAN, [test $html_man = yes])

AC_ARG_ENABLE(developer-doc,
	AS_HELP_STRING([--enable-developer-doc],
		       [Generate developer documentation using Doxygen [default=no]]),
	[dev_doc=$enableval], [dev_doc=no])
if [[ $dev_doc = yes -a x$DOXYGEN = x ]]; then
        AC_MSG_ERROR([Enabled generating developer documentation,
		      but Doxygen cannot be found!
		      Try --disable-developer-doc.])
fi
AM_CONDITIONAL(BUILD_DEVDOC, [test $dev_doc = yes])

AC_ARG_ENABLE(bootstrap,
	AS_HELP_STRING([--disable-bootstrap],
		       [Bootstrap using sciteco-minimal,
		        otherwise use preinstalled sciteco [default=yes]]),
	[bootstrap=$enableval], [bootstrap=yes])
if [[ $bootstrap = no -a x$SCITECO = x ]]; then
	AC_MSG_ERROR([Disabled bootstrapping, but preinstalled SciTECO cannot be found!
	              Try --enable-bootstrap.])
fi
AM_CONDITIONAL(BOOTSTRAP, [test x$bootstrap = xyes])

# This cannot be done with --enable-static as it only controls
# which kind of libraries libtool builds.
# Also, it cannot be controlled reliably by setting LDFLAGS for
# ./configure, as this would be used for linking the test cases
# without libtool and libtool would ignore it.
# It is only possible to call `make LDFLAGS="-all-static"` but
# this is inconvenient...
AC_ARG_ENABLE(static-executables,
	AS_HELP_STRING([--enable-static-executables],
		       [Link in as many runtime dependencies as possible
	                statically [default=no]]),
	[static_executables=$enableval], [static_executables=no])
AM_CONDITIONAL(STATIC_EXECUTABLES, [test x$static_executables = xyes])

AC_CONFIG_FILES([Makefile src/Makefile lib/Makefile])
AC_CONFIG_FILES([doc/Makefile doc/Doxyfile])
AC_OUTPUT
