SUBDIRS = src lib doc

dist_pkgdata_DATA = teco.ini

noinst_HEADERS = compat/bsd/sys/cdefs.h \
		 compat/bsd/sys/queue.h \
		 compat/bsd/sys/tree.h

EXTRA_DIST = $(wildcard @top_srcdir@/patches/*.patch) \
	     README

EXTRA_DIST += TODO
