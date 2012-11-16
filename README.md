Overview
========

SciTECO is an interactive TECO dialect, similar to [VideoTECO](http://www.copters.com/teco.html).
It also adds features from classic TECO-11, as well as unique new ideas.

The basic approach to text editing is both radically different and surprisingly similar to popular
editors like Vi or EMacs. Instead of using mostly keyboard commands to modify text in a visual
manner, in SciTECO you edit a program (called macro) using very few keyboard commands (called
immediate editing commands). This program edits text for the user and is executed immediately,
as far as possible. For instance, moving the cursor to the right can be done immediatly via the
"C" command which is a part of the editor language. The language is the editor so to speak.
When you delete a character from the end of the command line macro (called rubout), the
side-effects of that character which may be a command or part of a command, are undone.

SciTECO uses the [Scintilla](http://www.scintilla.org/) editor component and supports
GTK+ as well as NCurses frontends (using [Scinterm](http://foicica.com/scinterm/).

Features
========

Some of these features are not yet present but expected to be supported in the near future.

* All of the basic VideoTECO commands supported
* Operator precedence in arithmetic expressions and an argument stack that may be modified
  by the user (commands may have more than two arguments)
* Extended Q-Register namespace (arbitrary strings): can be used to build libraries and
  can be abused as a data structure
* Make use of your keyboard's function keys by assigning them to strings inserted into
  the command stream
* Many TECO-11 features, like that most commands have a colon-modified form, string-building
  characters, exotic match characters
* Interactivity: supports filename completion via immediate editing commands (e.g. `<TAB>` in
  commands accepting filenames); immediate searching (similar to search-as-you-type)
* Command rubout: SciTECO can even undo file writes (`EF` command)
* Munging: Macros may be munged, that is executed in batch mode. By default, a profile
  is munged.
* Syntax highlighting, styles, line numbers, etc. thanks to Scintilla

Installation
============

SciTECO has the following build-time dependencies:
* [Glib](http://developer.gnome.org/glib/), as a cross-platform runtime library
* [Scintilla](http://www.scintilla.org/), and possibly [Scinterm](http://foicica.com/scinterm/)
* [GTK+ 2](http://www.gtk.org/) or [NCurses](http://www.gnu.org/software/ncurses/).

1. Build Scintilla as described in its README. If you want to build the ncurses version,
   install Scinterm from a subdirectory of Scintilla called "scinterm".
2. Untar/unzip SciTECO into a subdirectory of Scintilla.
3. Build with `make INTERFACE=GTK` or `make INTERFACE=NCURSES` respectively.
3. Install with `make install`.
4. You are recommended to use the included "teco.ini" as a starting point for your profile,
   so copy it to "~/.config/.teco_ini". The default teco.ini doesn't do much currently,
   but it opens files specified on the shell commandline for you.

More documentation is coming soon.
