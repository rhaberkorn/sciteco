
SUBDIRS = src

if BOOTSTRAP
BOOTSTRAP_SCITECO = src/sciteco-minimal$(EXEEXT)
else
BOOTSTRAP_SCITECO = @SCITECO@
endif
REPL_MACRO = eb$<\e \
	     <fs@PACKAGE^Q@\e@PACKAGE@\e;>j \
	     <fs@PACKAGE_NAME^Q@\e@PACKAGE_NAME@\e;>j \
	     <fs@PACKAGE_VERSION^Q@\e@PACKAGE_VERSION@\e;>j \
	     <fs@PACKAGE_URL^Q@\e@PACKAGE_URL@\e;>j \
	     <fs@pkgdatadir^Q@\e$(pkgdatadir)\e;>j \
	     <fs@DATE^Q@\e$(shell @DATE@ "+%d %b %Y")\e;>j \
	     ew$@\e

pkgdata_DATA = teco.ini
EXTRA_DIST = teco.ini.in
CLEANFILES = teco.ini
dist_pkgdata_DATA = lexer.tes

man_MANS = sciteco.1
EXTRA_DIST += sciteco.1.in
CLEANFILES += sciteco.1

noinst_HEADERS = compat/bsd/sys/cdefs.h \
		 compat/bsd/sys/queue.h \
		 compat/bsd/sys/tree.h

EXTRA_DIST += patches/scintilla-teco-control-codes.patch

EXTRA_DIST += TODO

% : %.in
	$(BOOTSTRAP_SCITECO) -e $$'$(REPL_MACRO)'
