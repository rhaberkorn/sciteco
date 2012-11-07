GTK_CFLAGS:=$(shell pkg-config --cflags gtk+-2.0)
GTK_LDFLAGS:=$(shell pkg-config --libs gtk+-2.0)

SCI_CFLAGS:=-I../scintilla/include -DGTK -DSCI_LEXER
SCI_LDFLAGS:=../scintilla/bin/scintilla.a

CPPFLAGS:=
CXXFLAGS:=-Wall -g -O0 $(GTK_CFLAGS) $(SCI_CFLAGS)
LDFLAGS:=$(GTK_LDFLAGS) $(SCI_LDFLAGS)

all : sciteco

sciteco : main.o cmdline.o undo.o expressions.o parser.o
	$(CXX) -o $@ $^ $(LDFLAGS)

clean:
	$(RM) sciteco *.o
