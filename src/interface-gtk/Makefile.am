AM_CPPFLAGS += -I$(top_srcdir)/contrib/rb3ptr \
               -I$(top_srcdir)/src

AM_CFLAGS = -std=gnu11 -Wall -Wno-initializer-overrides -Wno-unused-value

noinst_LTLIBRARIES = libsciteco-interface.la
libsciteco_interface_la_SOURCES = interface.c \
                                  gtk-info-popup.c gtk-info-popup.h \
                                  gtk-label.c gtk-label.h

dist_scitecodata_DATA = fallback.css
