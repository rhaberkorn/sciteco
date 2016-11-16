# The Gtk and Curses UIs have their own subdirectories.
# Either of them will build libsciteco-interface.a
if INTERFACE_GTK
SUBDIRS = interface-gtk
LIBSCITECO_INTERFACE = interface-gtk/libsciteco-interface.la
else
SUBDIRS = interface-curses
LIBSCITECO_INTERFACE = interface-curses/libsciteco-interface.la
endif

include $(top_srcdir)/bootstrap.am
include $(top_srcdir)/scintilla.am

AM_CXXFLAGS = -std=c++11 -Wall -Wno-char-subscripts
if CLANG
AM_CXXFLAGS += -Wno-mismatched-tags
endif

if STATIC_EXECUTABLES
# AM_LDFLAGS are libtool flags, NOT compiler/linker flags
AM_LDFLAGS = -all-static
endif

BUILT_SOURCES =

EXTRA_DIST = symbols-extract.tes \
	     sciteco.html

noinst_LTLIBRARIES = libsciteco-base.la
libsciteco_base_la_SOURCES = main.cpp sciteco.h \
                             string-utils.cpp string-utils.h \
                             error.cpp error.h \
                             cmdline.cpp cmdline.h \
                             undo.cpp undo.h \
                             expressions.cpp expressions.h \
                             document.cpp document.h \
                             eol.cpp eol.h \
                             ioview.cpp ioview.h \
                             qregisters.cpp qregisters.h \
                             ring.cpp ring.h \
                             parser.cpp parser.h \
                             search.cpp search.h \
                             spawn.cpp spawn.h \
                             glob.cpp glob.h \
                             goto.cpp goto.h \
                             help.cpp help.h \
                             rbtree.cpp rbtree.h \
                             symbols.cpp symbols.h \
                             interface.cpp interface.h
# NOTE: We cannot link in Scintilla (static library) into
# a libtool convenience library
libsciteco_base_la_LIBADD = $(LIBSCITECO_INTERFACE)

if BOOTSTRAP
noinst_PROGRAMS = sciteco-minimal
symbols-scintilla.cpp symbols-scilexer.cpp : sciteco-minimal$(EXEEXT)
endif
sciteco_minimal_SOURCES = symbols-minimal.cpp
sciteco_minimal_LDADD = libsciteco-base.la \
                        @SCINTILLA_PATH@/bin/scintilla.a

bin_PROGRAMS = sciteco
sciteco_SOURCES =
nodist_sciteco_SOURCES = symbols-scintilla.cpp symbols-scilexer.cpp
sciteco_LDADD = $(sciteco_minimal_LDADD)

# For MinGW: Compile in resource (contains the icon)
if WIN32
sciteco_SOURCES += sciteco.rc
.rc.o:
	@RC@ $< $@
endif

CLEANFILES = $(BUILT_SOURCES) \
	     symbols-scintilla.cpp symbols-scilexer.cpp

symbols-scintilla.cpp : @SCINTILLA_PATH@/include/Scintilla.h \
			symbols-extract.tes
	$(SCITECO_MINIMAL) -m -- @srcdir@/symbols-extract.tes \
	                   $< $@ "SCI_" scintilla

symbols-scilexer.cpp : @SCINTILLA_PATH@/include/SciLexer.h \
		       symbols-extract.tes
	$(SCITECO_MINIMAL) -m -- @srcdir@/symbols-extract.tes \
	                   $< $@ "SCLEX_,SCE_" scilexer

# This installs a wrapper script to libexecdir to be used as
# the SciTECO interpreter in Hash-Bang lines.
# It makes sure that option parsing is disabled for all
# script arguments which is necessary for builds against Glib < 2.44.
# NOTE: When we raise the Glib requirement to 2.44, the sciteco-wrapper
# workaround can be removed completely.
libexec_SCRIPTS = sciteco-wrapper
CLEANFILES += $(libexec_SCRIPTS)

.PHONY: sciteco-wrapper
sciteco-wrapper:
	echo '#!/bin/sh' >$@
	echo 'OPT=$$1;' \
	     'shift;' \
	     "exec $(bindir)/`echo sciteco | @SED@ '$(transform)'`$(EXEEXT)" \
	     '"$$OPT" -- $$@' >>$@
