
# GTK or NCURSES
INTERFACE?=GTK

OS:=$(shell uname)

ifneq ($(findstring MINGW32,$(OS)),)
PREFIX?=/mingw
else
PREFIX?=/usr/local
endif

GOB2:=gob2
CC:=gcc
CXX:=g++
TOUCH:=touch
INSTALL:=install

GLIB_CFLAGS:=$(shell pkg-config --cflags glib-2.0)
GLIB_LDFLAGS:=$(shell pkg-config --libs glib-2.0)

SCI_DIR:=..
SCI_CFLAGS:=-I$(SCI_DIR)/include -D$(INTERFACE) -DSCI_LEXER
SCI_LDFLAGS:=$(SCI_DIR)/bin/scintilla.a

ifeq ($(INTERFACE),GTK)

GTK_CFLAGS:=$(shell pkg-config --cflags gtk+-2.0)
GTK_LDFLAGS:=$(shell pkg-config --libs gtk+-2.0)

else ifeq ($(INTERFACE),NCURSES)

SCI_CFLAGS+=-I$(SCI_DIR)/scinterm

ifeq ($(OS),Linux)
NCURSES_CFLAGS:=
NCURSES_LDFLAGS:=-lncurses
else
NCURSES_CFLAGS:=
NCURSES_LDFLAGS:=-lpdcurses
endif

endif

CPPFLAGS:=-DINTERFACE_$(INTERFACE)
CFLAGS:=-Wall -std=c99 -g -O0 \
	$(GLIB_CFLAGS) $(SCI_CFLAGS) $(GTK_CFLAGS) $(NCURSES_CFLAGS)
CXXFLAGS:=-Wall -g -O0 \
	  $(GLIB_CFLAGS) $(SCI_CFLAGS) $(GTK_CFLAGS) $(NCURSES_CFLAGS)
LDFLAGS:=$(GLIB_LDFLAGS) $(SCI_LDFLAGS) $(GTK_LDFLAGS) $(NCURSES_LDFLAGS)

ifneq ($(findstring MINGW32,$(OS)),)
CPPFLAGS+=-Icompat
endif

all : sciteco

sciteco : main.o cmdline.o undo.o expressions.o qbuffers.o \
	  parser.o goto.o rbtree.o \
	  interface.a
	$(CXX) -o $@ $^ $(LDFLAGS)

ifeq ($(INTERFACE),GTK)

interface.a : interface-gtk.o gtk-info-popup.o
	$(AR) rc $@ $^
	$(TOUCH) $@
interface-gtk.o : gtk-info-popup.h

else ifeq ($(INTERFACE),NCURSES)

interface.a : interface-ncurses.o
	$(AR) rc $@ $^
	$(TOUCH) $@

endif

%.c %.h %-private.h : %.gob
	$(GOB2) $<

install: all
	$(INSTALL) sciteco $(PREFIX)/bin

clean:
	$(RM) sciteco *.o *.a *.exe
	$(RM) gtk-info-popup*.[ch]
