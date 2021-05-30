AM_CPPFLAGS += -I$(top_srcdir)/contrib/rb3ptr \
               -I$(top_srcdir)/src

AM_CFLAGS = -std=gnu11 -Wall -Wno-initializer-overrides -Wno-unused-value

EXTRA_DIST = teco-gtk-info-popup.gob \
             teco-gtk-label.gob
BUILT_SOURCES = teco-gtk-info-popup.c teco-gtk-info-popup.h \
                teco-gtk-info-popup-private.h \
                teco-gtk-label.c teco-gtk-label.h

noinst_LTLIBRARIES = libsciteco-interface.la
libsciteco_interface_la_SOURCES = interface.c
nodist_libsciteco_interface_la_SOURCES = teco-gtk-info-popup.c \
                                         teco-gtk-label.c

dist_pkgdata_DATA = fallback.css

CLEANFILES = $(BUILT_SOURCES)

%.c %.h %-private.h : %.gob
	@GOB2@ --gtk3 $<
