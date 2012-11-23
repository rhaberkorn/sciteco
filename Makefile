
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

MINIMAL_OBJS:=main.o cmdline.o undo.o expressions.o qbuffers.o \
	      parser.o goto.o rbtree.o symbols.o \
	      interface.a

all : sciteco

sciteco : $(MINIMAL_OBJS) symbols-scintilla.o
	$(CXX) -o $@ $^ $(LDFLAGS)

sciteco-minimal : $(MINIMAL_OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS)

symbols-scintilla.cpp : $(SCI_DIR)/include/Scintilla.h \
			sciteco-minimal symbols-extract.tes
	./sciteco-minimal -m symbols-extract.tes $< $@ SCI_ scintilla

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
	$(RM) sciteco sciteco-minimal
	$(RM) *.o *.a *.exe
	$(RM) gtk-info-popup*.[ch] symbols-*.cpp
