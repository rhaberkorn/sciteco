AM_CPPFLAGS += -I$(top_srcdir)/src

AM_CXXFLAGS = -Wall -Wno-char-subscripts
if CLANG
AM_CXXFLAGS += -Wno-mismatched-tags
endif

noinst_LTLIBRARIES = libsciteco-interface.la
libsciteco_interface_la_SOURCES = interface-curses.cpp interface-curses.h
