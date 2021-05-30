AM_CPPFLAGS += -I$(top_srcdir)/contrib/rb3ptr \
               -I$(top_srcdir)/src

AM_CFLAGS = -std=gnu11 -Wall -Wno-initializer-overrides -Wno-unused-value

noinst_LTLIBRARIES = libsciteco-interface.la
libsciteco_interface_la_SOURCES = interface.c \
                                  curses-utils.c curses-utils.h \
                                  curses-info-popup.c curses-info-popup.h
