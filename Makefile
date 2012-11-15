
INTERFACE?=GTK

GOB2:=gob2

GLIB_CFLAGS:=$(shell pkg-config --cflags glib-2.0)
GLIB_LDFLAGS:=$(shell pkg-config --libs glib-2.0)

ifeq ($(INTERFACE),GTK)
GTK_CFLAGS:=$(shell pkg-config --cflags gtk+-2.0)
GTK_LDFLAGS:=$(shell pkg-config --libs gtk+-2.0)
else ifeq ($(INTERFACE),NCURSES)
# TODO
endif

SCI_CFLAGS:=-I../scintilla/include -D$(INTERFACE) -DSCI_LEXER
SCI_LDFLAGS:=../scintilla/bin/scintilla.a

CPPFLAGS:=-DINTERFACE_$(INTERFACE)
CFLAGS:=-Wall -std=c99 -g -O0 \
	$(GLIB_CFLAGS) $(GTK_CFLAGS) $(SCI_CFLAGS)
CXXFLAGS:=-Wall -g -O0 \
	  $(GLIB_CFLAGS) $(GTK_CFLAGS) $(SCI_CFLAGS)
LDFLAGS:=$(GLIB_LDFLAGS) $(GTK_LDFLAGS) $(SCI_LDFLAGS)

all : sciteco

sciteco : main.o cmdline.o undo.o expressions.o qbuffers.o \
	  parser.o goto.o rbtree.o \
	  interface.a
	$(CXX) -o $@ $^ $(LDFLAGS)

ifeq ($(INTERFACE),GTK)

interface.a : interface-gtk.o gtk-info-popup.o
	$(AR) rc $@ $^
interface-gtk.o : gtk-info-popup.h

else ifeq ($(INTERFACE),NCURSES)

# TODO
interface.a :
	$(AR) rc $@ $^

endif

%.c %.h %-private.h : %.gob
	$(GOB2) $<

clean:
	$(RM) sciteco *.o *.a
	$(RM) gtk-info-popup*.[ch]
