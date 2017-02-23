# Make sure that $BOOTSTRAP_SCITECO and the SciTECO-based
# substitutor is available.
include $(top_srcdir)/bootstrap.am

# tedoc is a code documentation tool for SciTECO commands
# and macros, extracting comments from SciTECO and C/C++
# code.
# It generates Troff manpage markup and acts as a Troff
# preprocessor to manpage templates.
dist_bin_SCRIPTS = tedoc.tes

%.in : %.template tedoc.tes $(SOURCES)
	$(SCITECO_FULL) -m -- @srcdir@/tedoc.tes \
	                -C $@ $< $(SOURCES)

# grosciteco is a troff postprocessor similar to grotty
# which can be used to produce SciTECO-friendly output
# (woman pages).
dist_bin_SCRIPTS += grosciteco.tes
# grosciteco, like many other postprocessors, works
# best with its own macro package.
# Unfortunately, there is no way to query the built-in
# Groff tmac path.
# Therefore, the "sciteco" macros are installed into
# SciTECO's pkgdatadir. Users can copy this file, set up
# environment variables like $GROFF_TMAC_PATH or pass
# the directory via `groff -M`.
dist_pkgdata_DATA = sciteco.tmac

# Fix up the hash-bang line of installed SciTECO scripts upon
# installation to refer to the installed sciteco binary.
# This takes --program-prefix into account.
install-exec-hook:
	for script in $(DESTDIR)$(bindir)/*.tes; do \
		@SED@ -i "1s,#!.*sciteco,#!$(libexecdir)/`echo sciteco-wrapper | @SED@ '$(transform)'`$(EXEEXT)," \
		      $$script; \
	done

womendir = $(scitecolibdir)/women

women_DATA = grosciteco.tes.1.woman grosciteco.tes.1.woman.tec
CLEANFILES = grosciteco.tes.1.intermediate

women_DATA += tedoc.tes.1.woman tedoc.tes.1.woman.tec
CLEANFILES += tedoc.tes.1.intermediate

women_DATA += sciteco.1.woman sciteco.1.woman.tec
CLEANFILES += sciteco.1.intermediate

women_DATA += sciteco.7.woman sciteco.7.woman.tec
CLEANFILES += sciteco.7.intermediate

CLEANFILES += $(women_DATA)

# NOTE: *.intermediate files are only generated since SciTECO scripts
# cannot currently read stdin, so the grosciteco postprocessor
# has to be run on a separate file.
%.woman %.woman.tec : %.intermediate grosciteco.tes
	$(SCITECO_FULL) -m -- @srcdir@/grosciteco.tes \
	                $@ $<

%.intermediate : % sciteco.tmac
	@GROFF@ -wall -Z -Tascii -t -man -M@srcdir@ -msciteco $< >$@

man_MANS = grosciteco.tes.1
EXTRA_DIST = grosciteco.tes.1.in

man_MANS += tedoc.tes.1
EXTRA_DIST += tedoc.tes.1.in

man_MANS += sciteco.1
EXTRA_DIST += sciteco.1.in

man_MANS += sciteco.7
EXTRA_DIST += sciteco.7.template
CLEANFILES += sciteco.7.in sciteco.7.htbl
SOURCES = $(wildcard @top_srcdir@/src/*.cpp)

CLEANFILES += $(man_MANS)

dist_noinst_SCRIPTS = htbl.tes

if BUILD_HTMLMAN
html_DATA = grosciteco.tes.1.html tedoc.tes.1.html \
            sciteco.1.html sciteco.7.html
CLEANFILES += $(html_DATA)
endif

# NOTE: The *.htbl files are only generated since SciTECO
# scripts cannot be integrated into pipelines easily yet.
%.htbl : % htbl.tes
	$(SCITECO_FULL) -m -- @srcdir@/htbl.tes $< $@

%.html : %.htbl
	@GROFF@ -wall -Thtml -man $< >$@

#
# Doxygen processing (do not install or distribute)
#
if BUILD_DEVDOC
noinst_DATA = doxygen/
endif

.PHONY: doxygen/
doxygen/ : Doxyfile
	-rm -rf doxygen/
	@DOXYGEN@ $<

clean-local:
	-rm -rf doxygen/
