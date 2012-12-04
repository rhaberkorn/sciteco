AM_CFLAGS = -Wall -std=c99
AM_CXXFLAGS = -Wall
AM_CPPFLAGS =
if NEED_COMPAT
AM_CPPFLAGS += -I@top_srcdir@/compat
endif

BUILT_SOURCES = symbols-scintilla.cpp symbols-scilexer.cpp

EXTRA_DIST = gtk-info-popup.gob \
	     symbols-extract.tes

noinst_PROGRAMS = sciteco-minimal
sciteco_minimal_SOURCES = main.cpp sciteco.h \
			  cmdline.cpp \
			  undo.cpp undo.h \
			  expressions.cpp expressions.h \
			  qregisters.cpp qregisters.h \
			  ring.cpp ring.h \
			  parser.cpp parser.h \
			  search.cpp search.h \
			  goto.cpp goto.h \
			  rbtree.cpp rbtree.h \
			  symbols.cpp symbols.h \
			  interface.h
nodist_sciteco_minimal_SOURCES =

if INTERFACE_GTK

BUILT_SOURCES += gtk-info-popup.c \
		 gtk-info-popup.h gtk-info-popup-private.h

sciteco_minimal_SOURCES += interface-gtk.cpp interface-gtk.h
nodist_sciteco_minimal_SOURCES += gtk-info-popup.c

else

sciteco_minimal_SOURCES += interface-ncurses.cpp interface-ncurses.h

endif

bin_PROGRAMS = sciteco
sciteco_SOURCES = $(sciteco_minimal_SOURCES)
nodist_sciteco_SOURCES = $(nodist_sciteco_minimal_SOURCES) \
			 symbols-scintilla.cpp symbols-scilexer.cpp

%.c %.h %-private.h : %.gob
	@GOB2@ $<

symbols-scintilla.cpp : @SCINTILLA_PATH@/include/Scintilla.h \
			sciteco-minimal symbols-extract.tes
	./sciteco-minimal -m symbols-extract.tes $< $@ "SCI_" scintilla

symbols-scilexer.cpp : @SCINTILLA_PATH@/include/SciLexer.h \
		       sciteco-minimal symbols-extract.tes
	./sciteco-minimal -m symbols-extract.tes $< $@ "SCLEX_,SCE_" scilexer
