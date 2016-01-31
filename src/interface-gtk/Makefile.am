AM_CPPFLAGS += -I$(top_srcdir)/src

AM_CFLAGS = -Wall -std=c99
AM_CXXFLAGS = -Wall -Wno-char-subscripts
if CLANG
AM_CXXFLAGS += -Wno-mismatched-tags
endif

EXTRA_DIST = gtk-info-popup.gob
BUILT_SOURCES = gtk-info-popup.c \
                gtk-info-popup.h gtk-info-popup-private.h

noinst_LTLIBRARIES = libsciteco-interface.la
libsciteco_interface_la_SOURCES = interface-gtk.cpp interface-gtk.h
if GTK_FLOW_BOX_FALLBACK
libsciteco_interface_la_SOURCES += gtkflowbox.c gtkflowbox.h
endif
nodist_libsciteco_interface_la_SOURCES = gtk-info-popup.c

CLEANFILES = $(BUILT_SOURCES)

%.c %.h %-private.h : %.gob
	@GOB2@ --gtk3 $<
