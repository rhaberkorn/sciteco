SUBDIRS = src lib doc

dist_pkgdata_DATA = sample.teco_ini

noinst_HEADERS = compat/bsd/sys/cdefs.h \
		 compat/bsd/sys/queue.h \
		 compat/bsd/sys/tree.h

EXTRA_DIST = README TODO

# Only the lower resolution PNG icons are installed as they are
# required by the GTK UI.
# Other uses are left to the distro package manager.
dist_pkgdata_DATA += ico/sciteco-16.png ico/sciteco-32.png \
                     ico/sciteco-48.png
EXTRA_DIST += ico/sciteco-256.png ico/sciteco.ico

# Distribute entire scintilla directory and
# do some manual cleanup
dist-hook:
	cp -r $(srcdir)/scintilla $(distdir)
	rm -rf `find $(distdir)/scintilla -name *.o` \
	       $(distdir)/scintilla/bin/scintilla.a
	rm -rf `find $(distdir)/scintilla -name ".git*"` \
	       `find $(distdir)/scintilla -name ".hg*"`
