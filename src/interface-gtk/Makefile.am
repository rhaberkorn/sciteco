AM_CPPFLAGS += -I$(top_srcdir)/src

AM_CFLAGS = -Wall
AM_CXXFLAGS = -Wall -Wno-char-subscripts

EXTRA_DIST = gtk-info-popup.gob \
             gtk-canonicalized-label.gob
BUILT_SOURCES = gtk-info-popup.c \
                gtk-info-popup.h gtk-info-popup-private.h \
                gtk-canonicalized-label.c \
                gtk-canonicalized-label.h

noinst_LTLIBRARIES = libsciteco-interface.la
libsciteco_interface_la_SOURCES = interface-gtk.cpp interface-gtk.h
if GTK_FLOW_BOX_FALLBACK
libsciteco_interface_la_SOURCES += gtkflowbox.c gtkflowbox.h
endif
nodist_libsciteco_interface_la_SOURCES = gtk-info-popup.c \
                                         gtk-canonicalized-label.c

dist_pkgdata_DATA = fallback.css

CLEANFILES = $(BUILT_SOURCES)

%.c %.h %-private.h : %.gob
	@GOB2@ --gtk3 $<
