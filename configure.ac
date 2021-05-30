#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.65])
AC_INIT([SciTECO], [2.0.0],
	[robin.haberkorn@googlemail.com],
	[sciteco],
	[http://sciteco.sf.net/])
PACKAGE_URL_DEV=https://github.com/rhaberkorn/sciteco
AC_SUBST(PACKAGE_URL_DEV)
AC_DEFINE_UNQUOTED(PACKAGE_URL_DEV, ["$PACKAGE_URL_DEV"],
                   [Package development homepage.])
AC_CONFIG_MACRO_DIR(m4)
AC_CONFIG_AUX_DIR(config)
AM_INIT_AUTOMAKE

AC_CONFIG_TESTDIR([tests])
AC_CONFIG_SRCDIR([src/main.c])
AC_CONFIG_HEADERS([config.h])

AC_CANONICAL_BUILD
AC_CANONICAL_HOST

# Use the user provided CXXFLAGS for Scintilla as well.
# This makes sure that the same optimizations are applied
# to SciTECO and Scintilla.
# We cannot pass CXXFLAGS directly to Scintilla since
# we modify it during library checking.
SCINTILLA_CXXFLAGS="$CXXFLAGS"
AC_SUBST(SCINTILLA_CXXFLAGS)

# Automake build flag substitutions.
# Necessary so we can change their default values here
AC_SUBST(AM_CPPFLAGS)

# SciTECO library macro directory
scitecolibdir=$datadir/$PACKAGE/lib
AC_SUBST(scitecolibdir)

# These paths can be changed at install-time and
# should not be written into config.h:
AM_CPPFLAGS="$AM_CPPFLAGS -D'SCITECODATADIR=\"\$(pkgdatadir)\"' \
                          -D'SCITECOLIBDIR=\"\$(scitecolibdir)\"'"

# Auxiliary functions

# expand $1 and print its absolute path
# similar to GNU readlink -e but portable
canonicalize() {
	(eval cd $1 2>/dev/null && pwd -P)
}

# Checks for programs.
LT_INIT
AC_PROG_CC
# FIXME: We want -std=gnu11
AC_PROG_CC_C99
AC_PROG_SED
AC_PROG_GREP
AC_PROG_INSTALL

# Mainly because of Scintilla, which depends on
# GNU Make.
# The main build system could be made POSIX-Make
# compliant, but that would complicate it and
# seems unnecessary.
# We prevent the main build system from being
# invoked with another Make by generating GNUmakefiles.
AX_CHECK_GNU_MAKE
if [[ x$ax_cv_gnu_make_command = x ]]; then
	AC_MSG_ERROR([GNU Make required!])
fi

# Additionally required by the Scintilla build process:
AC_PROG_CXX
AX_CXX_COMPILE_STDCXX(11, noext, mandatory)
AC_CHECK_TOOL(AR, ar)

# Whether $CC is Clang
AM_CONDITIONAL(CLANG, [$CC --version | $GREP -i clang >/dev/null])

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

# Changing the EXEEXT on emscripten ensures that we don't
# need a special Makefile rule to generate Javascript files.
AC_CHECK_DECL(EMSCRIPTEN, [
	EXEEXT=.js
])

AC_CHECK_PROG(DATE, date, date)
if [[ x$DATE = x ]]; then
	AC_MSG_ERROR([Required tool date not found!])
fi

# SciTECO's online help system is based on Troff.
# Its `grosciteco` postprocessor practically only works with GNU roff.
# There are other good Troff implementations like Heirloom Troff,
# but the postprocessor relies on ditroff generating
# intermediate output in TTY/nroff mode.
# In the original UNIX Troff, nroff is a distinct implementation
# that does not work with postprocessors.
#
# Groff may also be used to generate HTML versions of
# sciteco(1) and sciteco(7).
AC_CHECK_PROG(GROFF, groff, groff)
if [[ x$GROFF = x ]]; then
	AC_MSG_ERROR([GNU roff required!])
fi

# Doxygen is not necessarily required as long as
# you do not run `make devdoc`.
AC_CHECK_PROG(DOXYGEN, doxygen, doxygen)
AC_CHECK_PROG(DOT, dot, dot)
DOXYGEN_HAVE_DOT=YES
if [[ x$DOT = x ]]; then DOXYGEN_HAVE_DOT=NO; fi
AC_SUBST(DOXYGEN_HAVE_DOT)

# For bootstrapping via installed sciteco
AC_CHECK_PROG(SCITECO, sciteco, sciteco)

# Checks for libraries.
PKG_CHECK_MODULES(LIBGLIB, [glib-2.0 >= 2.44], [
	CFLAGS="$CFLAGS $LIBGLIB_CFLAGS"
	CXXFLAGS="$CXXFLAGS $LIBGLIB_CFLAGS"
	LIBS="$LIBS $LIBGLIB_LIBS"
])

# Checks for header files.
AC_HEADER_STDC

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
# They must exist on every target system.
AC_CHECK_FUNCS([memset setlocale strchr strrchr fstat sscanf], , [
	AC_MSG_ERROR([Missing libc function])
])

# Library functions that we assume exist on UNIX/Linux
# and UNIXoid systems, so that G_OS_UNIX is sufficient
# to test for them.
#
# NOTE: mmap() is currently only used by dlmalloc().
#
# FIXME: Perhaps it would be more elegant to check whether
# glib defines G_OS_UNIX instead...
case $host in
*-*-linux* | *-*-*bsd* | *-*-darwin* | *-*-cygwin* | *-*-haiku*)
	AC_CHECK_FUNCS([realpath fchown dup dup2 getpid open read mmap], , [
		AC_MSG_ERROR([Missing libc function])
	])
	;;
esac

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
	AS_HELP_STRING([--with-interface=ncurses|netbsd-curses|xcurses|pdcurses|gtk],
		       [Specify user interface [default=ncurses]]),
	[INTERFACE=$withval], [INTERFACE=ncurses])

case $INTERFACE in
*curses)
	case $INTERFACE in
	ncurses | netbsd-curses)
		PKG_CHECK_MODULES(NCURSES, [ncurses], [
			CFLAGS="$CFLAGS $NCURSES_CFLAGS"
			CXXFLAGS="$CXXFLAGS $NCURSES_CFLAGS"
			LIBS="$LIBS $NCURSES_LIBS"
		], [
			AC_CHECK_LIB(ncurses, initscr, , [
				AC_MSG_ERROR([libncurses missing!])
			])
		])
		AC_CHECK_FUNCS([tigetstr])

		if [[ x$INTERFACE = xnetbsd-curses ]]; then
			# NetBSD's curses can act as a ncurses
			# drop-in replacement and ships with a ncurses
			# pkg-config file. Still we define a symbol since
			# it's hard to detect at build-time.
			AC_DEFINE(NETBSD_CURSES, 1, [Building against netbsd-curses])
		fi
		;;

	xcurses)
		AC_CHECK_PROG(XCURSES_CONFIG, xcurses-config, xcurses-config)

		AC_ARG_VAR(XCURSES_CFLAGS, [
			C compiler flags for XCurses,
		        overriding the autoconf check
		])
		if [[ "x$XCURSES_CFLAGS" = "x" -a "x$XCURSES_CONFIG" != "x" ]]; then
			XCURSES_CFLAGS=`$XCURSES_CONFIG --cflags`
		fi
		CFLAGS="$CFLAGS $XCURSES_CFLAGS"
		CXXFLAGS="$CXXFLAGS $XCURSES_CFLAGS"

		AC_ARG_VAR(XCURSES_LIBS, [
			linker flags for XCurses,
		        overriding the autoconf check
		])
		AC_MSG_CHECKING([checking for XCurses])
		if [[ "x$XCURSES_LIBS" = "x" -a "x$XCURSES_CONFIG" != "x" ]]; then
			XCURSES_LIBS=`$XCURSES_CONFIG --libs`
		fi
		if [[ "x$XCURSES_LIBS" = "x" ]]; then
			AC_MSG_ERROR([libXCurses not configured correctly!
			              xcurses-config must be present or XCURSES_LIBS must be specified.])
		fi
		AC_MSG_RESULT([$XCURSES_LIBS])
		LIBS="$LIBS $XCURSES_LIBS"

		# This is also used by the curses.h itself:
		AC_DEFINE(XCURSES, 1, [Enable PDCurses/XCurses extensions])
		;;

	pdcurses)
		AC_ARG_VAR(PDCURSES_CFLAGS, [
			C compiler flags for PDCurses,
		        overriding the autoconf check
		])
		CFLAGS="$CFLAGS $PDCURSES_CFLAGS"
		CXXFLAGS="$CXXFLAGS $PDCURSES_CFLAGS"

		AC_ARG_VAR(PDCURSES_LIBS, [
			linker flags for PDCurses,
		        overriding the autoconf check
		])
		if [[ "x$PDCURSES_LIBS" = "x" ]]; then
			AC_CHECK_LIB(pdcurses, initscr, , [
				AC_MSG_ERROR([libpdcurses missing!
				              Perhaps it is not named correctly or has wrong permissions.])
			])
		else
			AC_MSG_CHECKING([checking for PDCurses (PDCURSES_LIBS)])
			AC_MSG_RESULT([$PDCURSES_LIBS])
			LIBS="$LIBS $PDCURSES_LIBS"
		fi

		AC_CHECK_FUNC([PDC_set_resize_limits], [
			AC_DEFINE(PDCURSES_WIN32A, 1, [PDCurses supports Win32a extensions])
		])
		;;
	esac

	AC_CHECK_HEADERS([curses.h], , [
		AC_MSG_ERROR([Curses header missing!])
	])
	AC_DEFINE(INTERFACE_CURSES, 1, [Build with curses support])

	# For Scintilla/Scinterm:
	CPPFLAGS="$CPPFLAGS -DCURSES -I$SCINTERM_PATH"
	;;

gtk)
	# gmodule is required by Scintilla.
	PKG_CHECK_MODULES(LIBGTK, [gtk+-3.0 >= 3.12 gmodule-2.0], [
		CFLAGS="$CFLAGS $LIBGTK_CFLAGS"
		CXXFLAGS="$CXXFLAGS $LIBGTK_CFLAGS"
		LIBS="$LIBS $LIBGTK_LIBS"
	])

	GOB2_CHECK(2.0.20)

	AC_DEFINE(INTERFACE_GTK, 1, [Build with GTK+ 3.0 support])

	# For Scintilla:
	CPPFLAGS="$CPPFLAGS -DGTK"
	;;

*)
	AC_MSG_ERROR([Invalid user interface specified!])
	;;
esac

AM_CONDITIONAL(INTERFACE_GTK, [test x$INTERFACE = xgtk])
AM_CONDITIONAL(GTK_FLOW_BOX_FALLBACK, [test x$GTK_FLOW_BOX_FALLBACK = xtrue])

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
AM_CONDITIONAL(BUILD_HTMLMAN, [test $html_man = yes])

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

AC_ARG_ENABLE(malloc-replacement,
	AS_HELP_STRING([--enable-malloc-replacement],
	               [Replace the libc malloc() [default=check]]),
	[malloc_replacement=$enableval], [malloc_replacement=check])
if [[ $malloc_replacement = check ]]; then
	# We currently do not support dlmalloc on Windows.
	case $host in
	*-mingw*) malloc_replacement=no;;
	*)        malloc_replacement=yes;;
	esac
fi
AM_CONDITIONAL(REPLACE_MALLOC, [test $malloc_replacement = yes])
if [[ $malloc_replacement = yes ]]; then
	AM_CPPFLAGS="$AM_CPPFLAGS -I\$(top_srcdir)/contrib/dlmalloc"
	AC_DEFINE(REPLACE_MALLOC, 1, [Define to 1 if the system malloc() is replaced.])
	AC_DEFINE(HAVE_MALLOC_H, 1, [Define to 1 if you have the <malloc.h> header file.])
	AC_DEFINE(HAVE_MALLOC_TRIM, 1, [Define to 1 if you have the `malloc_trim' function.])
else
	# NOTE: We don't check for malloc_footprint() since even if existing,
	# it cannot be guaranteed to work (eg. as in dlmalloc with HAVE_MORECORE=1).
	AC_CHECK_HEADERS([malloc.h malloc_np.h])
	AC_CHECK_FUNCS([malloc_trim], [
		AC_CHECK_DECLS([malloc_trim], , , [
			#include <malloc.h>
		])
	])

	case $host in
	*-mingw*)
		AC_CHECK_HEADERS([psapi.h], , [
			AC_MSG_ERROR([Missing Windows headers!])
		], [
			#include <windows.h>
		])

		# Make sure we get GetProcessMemoryInfo():
		AM_CPPFLAGS="$AM_CPPFLAGS -DPSAPI_VERSION=1"
		LIBS="$LIBS -lpsapi"
		;;
	*-*-linux*)
		AC_CHECK_HEADERS([sys/time.h sys/resource.h])
		AC_CHECK_FUNCS([sysconf])

		# FIXME: procfs might be available on other UNIXoid platforms.
		# Perhaps add a --with-procfs?
		# However, we currently also depend on sysconf().
		# Also, it should generally not be necessary to
		# --disable-malloc-replacement on UNIX.
		AC_DEFINE(HAVE_PROCFS, 1, [Whether procfs (/proc) is supported])
		;;
	*-*-*bsd*)
		AC_CHECK_HEADERS([sys/types.h sys/sysctl.h])
		AC_CHECK_FUNCS([sysctl])
		;;
	*-*-darwin*)
		AC_CHECK_HEADERS([mach/mach.h mach/message.h mach/kern_return.h mach/task_info.h])
		AC_CHECK_FUNCS([task_info mach_task_self])
		;;
	esac
fi

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

AC_CONFIG_FILES([GNUmakefile:Makefile.in src/GNUmakefile:src/Makefile.in]
                [src/interface-gtk/GNUmakefile:src/interface-gtk/Makefile.in]
                [src/interface-curses/GNUmakefile:src/interface-curses/Makefile.in]
                [contrib/dlmalloc/GNUmakefile:contrib/dlmalloc/Makefile.in]
                [contrib/rb3ptr/GNUmakefile:contrib/rb3ptr/Makefile.in]
                [lib/GNUmakefile:lib/Makefile.in]
                [doc/GNUmakefile:doc/Makefile.in doc/Doxyfile]
                [tests/GNUmakefile:tests/Makefile.in tests/atlocal])
AC_CONFIG_FILES([distribute.mk], [chmod +x distribute.mk])
AC_OUTPUT
