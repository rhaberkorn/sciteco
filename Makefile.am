include $(top_srcdir)/bootstrap.am

SUBDIRS = src doc

pkgdata_DATA = teco.ini
EXTRA_DIST = teco.ini.in
CLEANFILES = teco.ini
dist_pkgdata_DATA = lexer.tes

noinst_HEADERS = compat/bsd/sys/cdefs.h \
		 compat/bsd/sys/queue.h \
		 compat/bsd/sys/tree.h

EXTRA_DIST += patches/scintilla-teco-control-codes.patch

EXTRA_DIST += TODO
