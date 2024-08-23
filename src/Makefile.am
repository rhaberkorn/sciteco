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
include $(top_srcdir)/contrib/scintilla.am

# FIXME: Common flags should be in configure.ac
AM_CFLAGS = -std=gnu11 -Wall -Wno-initializer-overrides -Wno-unused-value
AM_CPPFLAGS += -I$(top_srcdir)/contrib/rb3ptr
AM_LDFLAGS =

if STATIC_EXECUTABLES
# AM_LDFLAGS are libtool flags, NOT compiler/linker flags
AM_LDFLAGS += -all-static
endif

BUILT_SOURCES =

dist_noinst_SCRIPTS = symbols-extract.tes

# Emscripten HTML page and Unix desktop file:
EXTRA_DIST = sciteco.html sciteco.desktop

noinst_LTLIBRARIES = libsciteco-base.la
libsciteco_base_la_SOURCES = main.c sciteco.h list.h \
                             memory.c memory.h \
                             string-utils.c string-utils.h \
                             file-utils.c file-utils.h \
                             error.c error.h \
                             cmdline.c cmdline.h \
                             undo.c undo.h \
                             expressions.c expressions.h \
                             doc.c doc.h \
                             eol.c eol.h \
                             qreg.c qreg.h \
                             qreg-commands.c qreg-commands.h \
                             ring.c ring.h \
                             parser.c parser.h \
                             core-commands.c core-commands.h \
                             search.c search.h \
                             spawn.c spawn.h \
                             glob.c glob.h \
                             goto.c goto.h \
                             goto-commands.c goto-commands.h \
                             help.c help.h \
                             rb3str.c rb3str.h \
                             symbols.c symbols.h \
                             view.c view.h \
                             interface.c interface.h
# NOTE: We cannot link in Scintilla (static library) into
# a libtool convenience library
libsciteco_base_la_LIBADD = $(LIBSCITECO_INTERFACE) \
                            $(top_builddir)/contrib/rb3ptr/librb3ptr.la
if REPLACE_MALLOC
libsciteco_base_la_LIBADD += $(top_builddir)/contrib/dlmalloc/libdlmalloc.la
endif

if BOOTSTRAP
noinst_PROGRAMS = sciteco-minimal
sciteco_minimal_SOURCES =
symbols-scintilla.c symbols-scilexer.c : sciteco-minimal$(EXEEXT)
endif
sciteco_minimal_LDADD = libsciteco-base.la $(LIBSCINTILLA)
if LEXILLA
sciteco_minimal_LDADD += $(LIBLEXILLA)
endif
# Scintilla is unfortunately still written in C++, so we must force
# Automake to use the C++ linker when linking the binaries.
# The following hack is actually advocated in the Automake manual.
nodist_EXTRA_sciteco_minimal_SOURCES = fuck-this-shit.cpp

bin_PROGRAMS = sciteco
sciteco_SOURCES =
nodist_sciteco_SOURCES = symbols-scintilla.c
if LEXILLA
nodist_sciteco_SOURCES += symbols-scilexer.c
endif
sciteco_LDADD = $(sciteco_minimal_LDADD)
# see above
nodist_EXTRA_sciteco_SOURCES = fuck-this-shit.cpp

# For MinGW: Compile in resource (contains the icon)
if WIN32
sciteco_SOURCES += sciteco.rc
.rc.o:
	@RC@ $< $@
endif

CLEANFILES = $(BUILT_SOURCES) \
	     symbols-scintilla.c symbols-scilexer.c

symbols-scintilla.c : @SCINTILLA_PATH@/include/Scintilla.h \
                      symbols-extract.tes
	$(SCITECO_MINIMAL) -m -- @srcdir@/symbols-extract.tes \
	                   -p "SCI_" -n teco_symbol_list_scintilla $@ $<

symbols-scilexer.c : @LEXILLA_PATH@/include/SciLexer.h \
                     symbols-extract.tes
	$(SCITECO_MINIMAL) -m -- @srcdir@/symbols-extract.tes \
	                   -p "SCE_" -n teco_symbol_list_scilexer $@ $<
