\# pdfmom --roff -U -P-pa4 -rW=19c -rO=1c -rL=31c -mm -mhdtbl -mpdfpic cheat-sheet.mm >cheat-sheet.pdf
.PGNH
.
.pdfinfo /Title  SciTECO Cheat Sheet
.pdfinfo /Author Robin Haberkorn
.
.SP 0.5c
.
.\" allow \fC instead of \f(CR.
.ftr C CR
.
.ad c
\#.B "SciTECO Cheat Sheet"
\#.SP
Overview of \fBSciTECO\fP as an editor.
A full language description can be found in
.pdfhref W -D https://rhaberkorn.github.io/sciteco/sciteco.7.html -A . \fBsciteco\fP(7)
.br
.
.\" subscripts
.ds < \v'+.3m\s'\En[.s]*7u/10u'-.1m'
.ds > \v'+.1m\s0-.3m'
.
.\" FIXME: For switching between CR and I fonts
.de CI
.  if \\n[.$] .fnt@switch \fC \fI \\$@
..
.
.\" For drawing with foreground ($1) and background ($2) color.
.\" Adapted from the BOX macro in Groff manual "Drawing Requests".
.\" The $ is added to standardize the height of all boxes
.\" (as it stretches above and below the base line).
.\" NOTE: This does not work in arguments to .TD!
.\" NOTE: It would ne nice to round the corners (as in Scintilla/Gtk's
.\" rendition of character representations), but there are no filled
.\" rounded polygons in Groff.
.ds FILLSTR \
\R!@wd \w'\\$3$'-\w'$'!\
\h'.1m'\
\h'-.1m'\v'(.1m - \\n[rsb]u)'\
\M[\\$2]\
\D'P 0 -(\\n[rst]u - \\n[rsb]u + .2m) \
     (\\n[@wd]u + .2m) 0 \
     0 (\\n[rst]u - \\n[rsb]u + .2m) \
     -(\\n[@wd]u + .2m) 0'\
\h'.1m'\v'-(.1m - \\n[rsb]u)'\
\M[]\
\m[\\$1]\\$3\m[]\
\h'.1m'
.
.\" Control char: monospaced font and inverted colors
.ds CTRL \fC\\*[FILLSTR white black "\\$*"]\fP
.ds $    \\*[CTRL $]
.ds $$   \\*[CTRL $$]
.
.ds t*hl
.ds t*vl
.nr t*csp 0
.ds t*bc  black
.ds t*fgc black
.ds t*bgc white
.ds t*hal l
.
.de TBLX
.  ds TBLX-TITLE \\$1
.  shift
.  TBL cols=2 \\$*
.    TR fgc=white bgc=black fst=B
.      TD colspan=2 "\\*[TBLX-TITLE]"
.  nr TRX 0 1
..
.de TRX
.  ie \\n+[TRX]%2 .TR bgc=grey90 \\$*
.  el .TR \\$*
..
.\" FIXME: Does not work when tables are automatically
.\" deferred to the next column or page.
.\" It's unclear what controls the spacing between tables.
.am ETB
.  sp -0.1c
..
.
.\" Legend
.TBL cols=4 width='10% 40% 10% 40%'
.  TR fgc=white bgc=black fst=B
.    TD colspan=4 "Legend"
.  TR bgc=grey90
.    TD
\*$
.    TD
String delimiter (Escape key)
\# Mention @ modifier?
\# Or that you can also press Delete if fnkeys.tes is loaded.
.    TD
\*($$
.    TD
Command-line termination (2\(muEscape key)
.  TR
.    TD
.      CI X
.    TD
Regular command.
They are case-insensitive.
.    TD
.      I n
.    TD
Some integer, often optional (1 or 0 by default).
You can write \fC\-\fP instead of \fC\-1\fP.
.  TR bgc=grey90
.    TD
.      I text
.    TD
Arbitrary \fItext\fP.
.    TD
.      I q
.    TD
A named storage area called a Q-register.
Use any case insensitive single character to name the register.
There are also two letter names initiated by \fC#\fP and long names in \fC[\fP...\fC]\fP braces.
.  TR
.    TD
\*[CTRL ^\f(CIX\fP]\fR
.    TD
Ctrl+\fIX\fP, but can also be typed with a caret (\fC^\fP).
.    TD
\*[CTRL LF]
.    TD
Line Feed, i.e. Enter/Return key
.  TR bgc=grey90
.    TD colspan=4
For instance:
\fC\-C\fP \(== \fC\-1C\fP \(== \fCR\fP \(DI
\fCQa\fP \(== \fCQ[A]\fP \(DI
\fCQ#ab\fP \(== \fCQ[AB]\fP \(DI
Ctrl+I \(== \*[CTRL ^I] \(== \*[CTRL TAB] \(DI
Ctrl+J \(== \*[CTRL ^J] \(== \*[CTRL LF]
.br
Undo (Rubout): Backspace, \*[CTRL ^W], \*[CTRL ^U] \(DI
Redo (Rubin): First \*[CTRL ^G], then Backspace, \*[CTRL ^W]...
.ETB
.
.\" Automatically move tables to the beginning of the next column.
.am tbl@top-hook
.  t*hm
..
.\" Automatically move tables to the beginning of the next page.
.am pg@end-of-text
.  t*EM
..
.
.MC (u;(\nW-0.5c)/3) 0.25c \" 3 columns
.
.TBLX "Exiting" width='30% 70%'
.  TRX
.    TD
\fCEX\fP\*($$
.    TD
Exit, but only if no buffer is \(lqdirty\(rq (unsaved)
.  TRX
.    TD
\fC\-EX\fP\*($$
.    TD
Exit even if buffer is \(lqdirty\(rq, i.e. discarding all unsaved changes.
.  TRX
.    TD
\fC:EX\fP\*($$
.    TD
Exit, saving all \(lqdirty\(rq buffers.
.ETB
.
.TBLX "Files" width='30% 70%'
.  TRX
.    TD
\fCEB\fI\^file\^\fR\*$
.    TD
Edit buffer or open new \fIfile\fP (glob pattern).
Files, that did not exist on disk, will not be created until you save them.
.  TRX
.    TD
\fCEB*.c\*$
.    TD
Open all files with extension \fCc\fP.
.  TRX
.    TD
\fCEB\fP\*$
.    TD
Edit the unnamed buffer.
.  TRX
.    TD
.      CI 0EB
.    TD
Show buffer ring/list.
You can specify a filename afterwards to open a file.
.  TRX
.    TD
\fIn\fCEB\fR\*$
.br
.CI "" n U*
.    TD
Select \fIn\fP-th buffer in ring.
.  TRX
.    TD
\fC%*\*$
.    TD
Select next buffer in ring.
.  TRX
.    TD
\fC\-%*\*$
.    TD
Select previous buffer in ring.
.  TRX
.    TD
.      CI EJU*
.    TD
Select last buffer in ring.
.  TRX
.    TD
\fCEW\fP\*$
.    TD
Write (save) current buffer under its current name.
Does not work on the unnamed buffer.
.  TRX
.    TD
\fCEW\fI\^file\^\*$
.    TD
Save current buffer under new name \fIfile\fP (Save As).
.  TRX
.    TD
.      CI EF
.    TD
Finish (close) current buffer.
.  TRX
.    TD
.      CI \-EF
.    TD
Finish (close) current buffer, discarding all unsaved changes.
.  TRX
.    TD
\fCFG\fI\^path\^\fR\*$
.    TD
Go to folder \fIpath\fP, i.e. change working directory.
.  TRX
.    TD
.      CI 0EE
.    TD
Set single byte ASCII mode.
.  TRX
.    TD colspan=2
\fBTip:\fP You can use the Tab-key for autocompleting filenames and paths.
.ETB
.
.NCOL
.
.TBLX "Text Insertion" width='30% 70%'
.  TRX
.    TD
\fCI\fItext\^\*$
.    TD
Insert \fItext\fP into buffer.
.  TRX
.    TD
.      CI I ... ^^
.    TD
Insert single caret (\fC^\fP).
.  TRX
.    TD
\fCI\fI...\*[CTRL ^Q$]
.    TD
Insert \*$ (ASCII 27).
.  TRX
.    TD
\*[CTRL TAB]\fI\^text\^\*$
.    TD
Insert \fItext\fP with leading tab/indentation.
See also
.pdfhref W -D https://github.com/rhaberkorn/sciteco/wiki/Useful-Macros#indent-code-block -A . \fIn\^\fCM#it\fP
.ETB
.
.TBLX "Text Deletion" width='30% 70%'
.  TRX
.    TD
.      CI D
.    TD
Delete next character.
.  TRX
.    TD
.      CI "" n D
.    TD
Delete next \fIn\fP characters.
.ig END
.  TRX
.    TD
.      CI V
.    TD
Delete next word.
.END
.  TRX
.    TD
.      CI "" n V
.    TD
Delete next \fIn\fP words.
.  TRX
.    TD
.      CI "" n Y
.    TD
Delete previous \fIn\fP words.
.  TRX
.    TD
\fCFK\*[CTRL LF$]
.    TD
Delete remainder of line.
.ig END
.  TRX
.    TD
.      CI K
.    TD
Kill (delete) from current position to beginning of next line.
.END
.  TRX
.    TD
.      CI 0K
.    TD
Kill (delete) to beginning of current line.
.  TRX
.    TD
.      CI 0KK
.    TD
Kill (delete) entire line
.  TRX
.    TD
.      CI "" n K
.    TD
Kill (delete) next \fIn\fP lines.
.  TRX
.    TD
.      CI HK
.    TD
Kill (delete) whole buffer.
.ETB
.
.TBLX "Copy & Paste" width='30% 70%'
.  TRX
.    TD
.      CI X q
.    TD
Copy from current position until beginning of next line into Q-Register \fIq\fP.
.  TRX
.    TD
.      CI "" n X q
.    TD
Copy next \fIn\fP lines into Q-Register \fIq\fP.
.  TRX
.    TD
.      CI "" n :X q
.    TD
Append next \fIn\fP lines to Q-Register \fIq\fP.
.  TRX
.    TD
.      CI "" n @X q
.    TD
Cut next \fIn\fP lines into Q-Register \fIq\fP.
.  TRX
.    TD
.      CI HX q
.    TD
Copy whole buffer into Q-Register \fIq\fP.
.  TRX
.    TD
.      CI X\(ti
.    TD
Copy line into clipboard. See also
.pdfhref W -D https://github.com/rhaberkorn/sciteco/wiki/Useful-Macros#copypaste-from-clipboard -A . \fCM#xc\fP
.  TRX
.    TD
.      CI G q
.    TD
Get (paste) Q-Register \fIq\fP at current position.
.  TRX
.    TD
\fCI\fI...\*[CTRL ^E]\fCQ\fI\^q
.    TD
Paste Q-Register \fIq\fP while inserting text.
.ig END
.  TRX
.    TD
\fCE%\fIq\|file\^\*$
.    TD
Save Q-Register \fIq\fP into \fIfile\fP.
.END
.  TRX
.    TD
\fCEQ\fI\^q\^\*$
.    TD
Edit Q-Register \fIq\fP as a text buffer.
.ig END
.  TRX
.    TD
\fCEQ\fIq\|file\*$
.    TD
Read \fIfile\fP into Q-Register \fIq\fP.
.END
.ETB
.
.NCOL
.
.TBLX "Cursor Movement" width='30% 70%'
.  TRX
.    TD
.      CI C
.    TD
Move one character forward.
.  TRX
.    TD
.      CI "" n C
.    TD
Move \fIn\fP characters forward.
.  TRX
.    TD
.      CI R
.    TD
Move one character backwards (reverse).
.  TRX
.    TD
.      CI "" n R
.    TD
Move \fIn\fP characters backwards (reverse).
.  TRX
.    TD
.      CI W
.    TD
Move to the beginning of next word.
.  TRX
.    TD
.      CI L
.    TD
Move to the beginning of next line.
.  TRX
.    TD
.      CI "" n L
.    TD
Move forward \fIn\fP lines.
.  TRX
.    TD
.      CI 0L
.    TD
Move to the beginning of current line.
.  TRX
.    TD
.      CI LR
.    TD
Move to end of current line.
.  TRX
.    TD
.      CI "" n B
.    TD
Move backwards \fIn\fP lines.
.  TRX
.    TD
.      CI J n L
.    TD
Go to beginning of line \fIn\fP+1.
.  TRX
.    TD
.      CI J
.    TD
Jump to beginning of buffer.
.  TRX
.    TD
.      CI ZJ
.    TD
Jump to end of buffer.
.  TRX
.    TD colspan=2
\fBTip:\fP Enable the \fCfnkeys.tes\fP module in \fC.teco_ini\fP
to move around with cursor keys!
.ETB
.
.TBLX "External Programs" width='42% 58%'
.  TRX
.    TD
\fCEC\fI\^command\^\*$
.    TD
Insert output of \fIcommand\fP.
.  TRX
.    TD
\fIn\fCEC\fI\^command\^\*$
.    TD
Filter next \fIn\fP lines through \fIcommand\fP.
.  TRX
.    TD
\fCHEC\fI\^command\^\*$
.    TD
Filter whole buffer through \fIcommand\fP.
.  TRX
.    TD
\fIn\fCECsort\*$
.    TD
Sort next \fIn\fP lines (UNIX).
.ETB
.
.TBLX "Macros" width='42% 58%'
.  TRX
.    TD
\fC@\*[CTRL ^U]\fI\^q\fP{\fImacro\fP}
.    TD
Define \fImacro\fP in Q-Register \fIq\fP.
.  TRX
.    TD
.      CI M \^q
.    TD
Call macro in Q-Register \fIq\fP.
.  TRX
.    TD
\*($$\fC*\fIq
.    TD
Discard command-line, storing it in \fIq\fP.
.ETB
.
.PGNH
.\" FIXME: We shouldn't have to reinitialize the column mode.
.1C \" Backpage
.SP 0.5c
.
.MC (u;(\nW-0.5c)/3) 0.25c \" 3 columns
.
.TBLX "Search & Replace" width='47% 53%'
.  TRX
.    TD
\fCS\fItext\^\*$
.    TD
Search for next occurrence of \fItext\fP.
.  TRX
.    TD
\fCS\fItext\^\*[CTRL $^S]\fCC
.    TD
Search for beginning of \fItext\fP.
.  TRX
.    TD
\fC\-S\fItext\^\*$
.    TD
Search for previous occurrence of \fItext\fP.
.  TRX
.    TD
\fIn\fCS\fItext\^\*$
.    TD
Search for \fIn\fP-th occurrence of \fItext\fP.
.  TRX
.    TD
\fCS\*$
.    TD
Repeat last search (pattern from Q-Register \fC_\fP).
.  TRX
.    TD
\fCN\fItext\^\*$
.    TD
Search for next occurrence of \fItext\fP across all buffers.
.  TRX
.    TD
\fCFR\fI\^from\^\*$\fI\^to\^\*$
.    TD
Find next occurrence of \fIfrom\fP and replace it with \fIto\fP.
.  TRX
.    TD
\fCFR\*($$
.    TD
Repeat the last search-replace operation (\fC_\fP and \fC\-\fP).
.  TRX
.    TD
\fC<FR\fI\^from\^\*$\fI\^to\^\*$\fC;>\fP
.    TD
Find and replace all occurrences in buffer beginning at current position.
.  TRX
.    TD
\fCFK\fItext\^\*$
.    TD
Find and kill (delete) up to first occurrence of \fItext\fP.
.  TRX
.    TD
\fCFD\fItext\^\*$
.    TD
Find and delete first occurrence of \fItext\fP.
.ETB
.
.TBLX "Control Flow" width='47% 53%'
.  TRX
.    TD
.      CI < commands >
.    TD
Repeat \fIcommands\fP infinitely.
.  TRX
.    TD
.      CI "" n < commands >
.    TD
Repeat \fIcommands\fP \fIn\fP times.
.  TRX
.    TD
.      CI "" n ;
.    TD
Break from loop if \fIn\fP is false (non-negative).
.  TRX
.    TD colspan=2
For instance, to add \fC#\fP in front of the next 10 lines:
\fC0L10<I#\*$L>\fP
.ETB
.
.TBLX "Help" width='47% 53%'
.  TRX
.    TD
\fC?\fItopic\^\*$
.    TD
Search help for \fItopic\fP (may be command).
.ig END \" not yet supported
.  TRX
.    TD
\fC?\*$
.    TD
Search help by word at current position in buffer.
.END
.  TRX
.    TD colspan=2
\fBTip:\fP You can use the Tab-key for autocompleting topics.
.ETB
.
.NCOL
.
.TBLX "Search Patterns" width='40% 60%'
.  TRX
.    TD
\*[CTRL ^X]
.    TD
Matches any character.
.  TRX
.    TD
\*[CTRL ^E]\fCS
.    TD
Matches any non-empty sequence of whitespace characters.
.  TRX
.    TD
\*[CTRL ^E]\fCA
.    TD
Matches any alphabetic characters.
.  TRX
.    TD
\*[CTRL ^E]\fCD
.    TD
Matches any digit.
.  TRX
.    TD
\*[CTRL ^N]\fI\^class\fP
.    TD
Matches any character not in \fIclass\fP.
.  TRX
.    TD
\*[CTRL ^E]\fCM\fI\^pattern\fP
.    TD
Matches many occurrences of \fIpattern\fP.
.  TRX
.    TD
\*[CTRL ^E]\fCG\fI\^q\fP
.    TD
Matches any character in Q-Register \fIq\fP.
.  TRX
.    TD
\*[CTRL ^E]\fC[\fIp\*<1\*>\fP,\fIp\*<2\*>\fP,\fI...\fP]
.    TD
Matches \fIp\*<1\*>\fP or \fIp\*<2\*>\fP.
.  TRX
.    TD colspan=2
To remove all trailing whitespace characters, you could type:
.br
\fCJ<FR\*[CTRL ^E]M\*[CTRL ^E][\0,\*[CTRL TAB]]\*[CTRL LF$LF$];>
.ETB
.
.TBLX "String Building" width='40% 60%'
.  TRX
.    TD
\*[CTRL ^E]\fCQ\fI\^q
.    TD
Expand to string contents of Q-Register \fIq\fP.
.  TRX
.    TD
\*[CTRL ^E]\fC\\\fIq
.    TD
Expand to integer contents of Q-Register \fIq\fP.
.  TRX
.    TD
\*[CTRL ^E]\fCU\fIq
.    TD
Expand to character represented by codepoint in Q-Register \fIq\fP.
.  TRX
.    TD
\*[CTRL ^Q]\fI\^x
.    TD
Quote (escape) the following character \fIx\fP.
.  TRX
.    TD
\*[CTRL ^Q^Q]
.    TD
Expands to \*[CTRL ^Q].
.ETB
.
.\" Perhaps add the rubout/rubin code as well?
.\" If so, it would rather belong on page 1.
.TBLX "Command-line Editing" width='40% 60%'
.  TRX
.    TD
.      CI {
.    TD
Edit current command-line.
.  TRX
.    TD
.      CI }
.    TD
Replace command-line with edited version.
.  TRX
.    TD
.      CI {HK}
.    TD
Undo the entire command-line.
.ig END
.  TRX
.    TD
\*[CTRL ^W]
.    TD
Rub out word or command.
.  TRX
.    TD
\*[CTRL ^U]
.    TD
Rub out string argument.
.END
.  TRX
.    TD
\*[CTRL ^G^W]
.    TD
Rub in word.
Also try Shift+Delete if \fCfnkeys.tes\fP is loaded.
.ETB
.
.sp |(u;\nL-7.7c)
.PDFPIC -I -5c ../ico/sciteco-256.pdf 5c
.
.NCOL
.
.TBLX "Arithmetics" width='40% 60%'
.  TRX
.    TD
.      CI "" n U q
.    TD
Assign number \fIn\fP to Q-Register \fIq\fP.
.  TRX
.    TD
.      CI \-U q
.    TD
Assign -1 to Q-Register \fIq\fP.
.  TRX
.    TD
.      CI Q \^q
.    TD
Query (get) integer from Q-Register \fIq\fP.
.  TRX
.    TD
.      CI "" n % q
.    TD
Add \fIn\fP to Q-Register \fIq\fP and return new value.
.  TRX
.    TD
.      CI % q
.    TD
Increase Q-Register \fIq\fP and return new value.
.  TRX
.    TD
.      CI \-% q
.    TD
Decrease Q-Register \fIq\fP and return new value.
.  TRX
.    TD
\*[CTRL ^^]\fI\^x
.    TD
Codepoint of character \fIx\fP.
.  TRX
.    TD
.      CI "" n A
.    TD
Get codepoint \fIn\fP characters after current position.
.  TRX
.    TD
\fC\\
.    TD
Parse and retrieve integer at current position in buffer.
.  TRX
.    TD
\fIn\fC\\
.    TD
Insert integer \fIn\fP into buffer at current position.
.  TRX
.    TD
\fC\\+\fIn\fP\\V
.    TD
Add \fIn\fP to number at current position in buffer.
.  TRX
.    TD
\fIn\^\*[CTRL ^_]
.    TD
Binary negate \fIn\fP \(em negate TECO boolean.
.  TRX
.    TD
.      CI "" n =
.    TD
Show value of \fIn\fP in message line.
.  TRX
.    TD colspan=2
Q-Registers consist of 2 cells: strings and integers.
These are independent.
Setting a number does not change the string part!
.ETB
.
.TBLX "Syntax Highlighting (lexers.tes)" width='61% 39%'
.  TRX
.    TD
.      CI M[lexer.set. name ]
.    TD
Set lexer (syntax highlighting) for language \fIname\fP.
.  TRX
.    TD colspan=2
\fBTip:\fP You can use the Tab-key for autocompleting long Q-Register
names (and therefore Lexer names).
.ETB
\# EOF