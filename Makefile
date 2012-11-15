GOB2:=gob2

GTK_CFLAGS:=$(shell pkg-config --cflags gtk+-2.0)
GTK_LDFLAGS:=$(shell pkg-config --libs gtk+-2.0)

SCI_CFLAGS:=-I../scintilla/include -DGTK -DSCI_LEXER
SCI_LDFLAGS:=../scintilla/bin/scintilla.a

CPPFLAGS:=-DINTERFACE_GTK
CFLAGS:=-Wall -std=c99 -g -O0 $(GTK_CFLAGS) $(SCI_CFLAGS)
CXXFLAGS:=-Wall -g -O0 $(GTK_CFLAGS) $(SCI_CFLAGS)
LDFLAGS:=$(GTK_LDFLAGS) $(SCI_LDFLAGS)

all : sciteco

sciteco : main.o cmdline.o undo.o expressions.o qbuffers.o \
	  parser.o goto.o rbtree.o \
	  interface-gtk.o gtk-info-popup.o
	$(CXX) -o $@ $^ $(LDFLAGS)

main.o : gtk-info-popup.h

%.c %.h %-private.h : %.gob
	$(GOB2) $<

clean:
	$(RM) sciteco *.o
	$(RM) gtk-info-popup*.[ch]
