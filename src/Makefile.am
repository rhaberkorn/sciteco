include $(top_srcdir)/bootstrap.am
include $(top_srcdir)/scintilla.am

AM_CFLAGS = -Wall -std=c99
AM_CXXFLAGS = -Wall -Wno-char-subscripts
if CLANG
AM_CXXFLAGS += -Wno-mismatched-tags
endif

# These paths can be changed at install-time and
# should not be written into config.h:
AM_CPPFLAGS = -D'SCITECODATADIR="$(pkgdatadir)"' \
              -D'SCITECOLIBDIR="@scitecolibdir@"'

if NEED_COMPAT
AM_CPPFLAGS += -I@top_srcdir@/compat
endif

if STATIC_EXECUTABLES
# AM_LDFLAGS are libtool flags, NOT compiler/linker flags
AM_LDFLAGS = -all-static
endif

BUILT_SOURCES =

EXTRA_DIST = gtk-info-popup.gob \
	     symbols-extract.tes \
	     sciteco.html

noinst_LIBRARIES = libsciteco-base.a
libsciteco_base_a_SOURCES = main.cpp sciteco.h \
			    string-utils.cpp string-utils.h \
			    error.cpp error.h \
			    cmdline.cpp cmdline.h \
			    undo.cpp undo.h \
			    expressions.cpp expressions.h \
			    document.cpp document.h \
			    ioview.cpp ioview.h \
			    qregisters.cpp qregisters.h \
			    ring.cpp ring.h \
			    parser.cpp parser.h \
			    search.cpp search.h \
			    spawn.cpp spawn.h \
			    glob.cpp glob.h \
			    goto.cpp goto.h \
			    rbtree.cpp rbtree.h \
			    symbols.cpp symbols.h \
			    interface.cpp interface.h
nodist_libsciteco_base_a_SOURCES =

if INTERFACE_GTK

BUILT_SOURCES += gtk-info-popup.c \
		 gtk-info-popup.h gtk-info-popup-private.h
nodist_libsciteco_base_a_SOURCES += gtk-info-popup.c

libsciteco_base_a_SOURCES += interface-gtk.cpp interface-gtk.h
if GTK_FLOW_BOX_FALLBACK
libsciteco_base_a_SOURCES += gtkflowbox.c gtkflowbox.h
endif

else
# else must be Curses interface

libsciteco_base_a_SOURCES += interface-curses.cpp interface-curses.h

endif

if BOOTSTRAP
noinst_PROGRAMS = sciteco-minimal
symbols-scintilla.cpp symbols-scilexer.cpp : sciteco-minimal$(EXEEXT)
endif
sciteco_minimal_SOURCES = symbols-minimal.cpp
sciteco_minimal_LDADD = libsciteco-base.a \
			@SCINTILLA_PATH@/bin/scintilla.a

bin_PROGRAMS = sciteco
sciteco_SOURCES =
nodist_sciteco_SOURCES = $(nodist_sciteco_minimal_SOURCES) \
			 symbols-scintilla.cpp symbols-scilexer.cpp
sciteco_LDADD = $(sciteco_minimal_LDADD)

# For MinGW: Compile in resource (contains the icon)
if WIN32
sciteco_SOURCES += sciteco.rc
.rc.o:
	@RC@ $< $@
endif

CLEANFILES = $(BUILT_SOURCES) \
	     symbols-scintilla.cpp symbols-scilexer.cpp

%.c %.h %-private.h : %.gob
	@GOB2@ --gtk3 $<

symbols-scintilla.cpp : @SCINTILLA_PATH@/include/Scintilla.h \
			symbols-extract.tes
	$(BOOTSTRAP_SCITECO) -m @srcdir@/symbols-extract.tes \
			     $< $@ "SCI_" scintilla

symbols-scilexer.cpp : @SCINTILLA_PATH@/include/SciLexer.h \
		       symbols-extract.tes
	$(BOOTSTRAP_SCITECO) -m @srcdir@/symbols-extract.tes \
			     $< $@ "SCLEX_,SCE_" scilexer
