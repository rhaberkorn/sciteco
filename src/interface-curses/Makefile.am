AM_CPPFLAGS += -I$(top_srcdir)/src

AM_CXXFLAGS = -std=c++11 -Wall -Wno-char-subscripts
if CLANG
AM_CXXFLAGS += -Wno-mismatched-tags
endif
if HAVE_SIZED_DEALLOCATION
AM_CXXFLAGS += -fsized-deallocation
endif

noinst_LTLIBRARIES = libsciteco-interface.la
libsciteco_interface_la_SOURCES = interface-curses.cpp interface-curses.h \
                                  curses-utils.cpp curses-utils.h \
                                  curses-info-popup.cpp curses-info-popup.h
