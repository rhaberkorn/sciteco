SUBDIRS = src lib doc

dist_pkgdata_DATA = sample.teco_ini

noinst_HEADERS = compat/bsd/sys/cdefs.h \
		 compat/bsd/sys/queue.h \
		 compat/bsd/sys/tree.h

EXTRA_DIST = README TODO

# The icons are not installed currently.
# This should be left up to the Linux distro
# packager.
EXTRA_DIST += ico/sciteco-48.png \
              ico/sciteco.ico

# Distribute entire scintilla directory and
# do some manual cleanup
EXTRA_DIST += scintilla
dist-hook:
	rm -rf `find $(distdir)/scintilla -name *.o` \
	       $(distdir)/scintilla/bin/scintilla.a
	rm -rf `find $(distdir)/scintilla -name ".git*"` \
	       `find $(distdir)/scintilla -name ".hg*"`
