# Required in addition to AC_CONFIG_MACRO_DIR() to
# silence libtoolize:
ACLOCAL_AMFLAGS = -I m4

if REPLACE_MALLOC
MAYBE_DLMALLOC = contrib/dlmalloc
endif
SUBDIRS = lib $(MAYBE_DLMALLOC) contrib/rb3ptr src doc tests

dist_scitecodata_DATA = sample.teco_ini

EXTRA_DIST = README TODO

# Only the lower resolution PNG icons are installed as they are
# required by the GTK UI.
# Other uses are left to the distro package manager.
dist_scitecodata_DATA += ico/sciteco-16.png ico/sciteco-32.png \
                         ico/sciteco-48.png
EXTRA_DIST += ico/sciteco-256.png ico/sciteco.ico

EXTRA_DIST += win32/loaders.cache

EXTRA_DIST += contrib/mingw-bundledlls

# Distribute entire Scintilla/Scinterm/Lexilla directory and
# do some manual cleanup.
dist-hook:
	cp -r $(srcdir)/contrib/scintilla $(distdir)/contrib
	cp -r $(srcdir)/contrib/scinterm $(distdir)/contrib
	cp -r $(srcdir)/contrib/lexilla $(distdir)/contrib
	rm -rf `find $(distdir)/contrib -name *.$(OBJEXT)` \
	       `find $(distdir)/contrib -name ".git*"` \
	       $(distdir)/contrib/scintilla/bin/* \
	       $(distdir)/contrib/lexilla/bin/*
