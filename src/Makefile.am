include $(top_srcdir)/bootstrap.am

AM_CFLAGS = -Wall -std=c99
AM_CXXFLAGS = -Wall -Wno-mismatched-tags -Wno-char-subscripts
AM_CPPFLAGS = -D'DEFAULT_SCITECOPATH="@DEFAULT_SCITECOPATH@"'
if NEED_COMPAT
AM_CPPFLAGS += -I@top_srcdir@/compat
endif

BUILT_SOURCES = symbols-scintilla.cpp symbols-scilexer.cpp

EXTRA_DIST = gtk-info-popup.gob \
	     symbols-extract.tes

noinst_LIBRARIES = libsciteco-base.a
libsciteco_base_a_SOURCES = sciteco.h \
			    cmdline.cpp cmdline.h \
			    undo.cpp undo.h \
			    expressions.cpp expressions.h \
			    document.cpp document.h \
			    qregisters.cpp qregisters.h \
			    ring.cpp ring.h \
			    parser.cpp parser.h \
			    search.cpp search.h \
			    goto.cpp goto.h \
			    rbtree.cpp rbtree.h \
			    symbols.cpp symbols.h \
			    interface.h
nodist_libsciteco_base_a_SOURCES =

if INTERFACE_GTK

BUILT_SOURCES += gtk-info-popup.c \
		 gtk-info-popup.h gtk-info-popup-private.h

libsciteco_base_a_SOURCES += interface-gtk.cpp interface-gtk.h
nodist_libsciteco_base_a_SOURCES += gtk-info-popup.c

else
# else must be NCurses interface

libsciteco_base_a_SOURCES += interface-ncurses.cpp interface-ncurses.h

endif

if BOOTSTRAP
noinst_PROGRAMS = sciteco-minimal
symbols-scintilla.cpp symbols-scilexer.cpp : sciteco-minimal$(EXEEXT)
endif
sciteco_minimal_SOURCES = main.cpp
sciteco_minimal_LDADD = libsciteco-base.a \
			@SCINTILLA_PATH@/bin/scintilla.a

bin_PROGRAMS = sciteco
sciteco_SOURCES = $(sciteco_minimal_SOURCES)
nodist_sciteco_SOURCES = $(nodist_sciteco_minimal_SOURCES) \
			 symbols-scintilla.cpp symbols-scilexer.cpp
sciteco_LDADD = $(sciteco_minimal_LDADD)

CLEANFILES = $(BUILT_SOURCES)

%.c %.h %-private.h : %.gob
	@GOB2@ $<

symbols-scintilla.cpp : @SCINTILLA_PATH@/include/Scintilla.h \
			symbols-extract.tes
	$(BOOTSTRAP_SCITECO) -m @srcdir@/symbols-extract.tes \
			     $< $@ "SCI_" scintilla

symbols-scilexer.cpp : @SCINTILLA_PATH@/include/SciLexer.h \
		       symbols-extract.tes
	$(BOOTSTRAP_SCITECO) -m @srcdir@/symbols-extract.tes \
			     $< $@ "SCLEX_,SCE_" scilexer
