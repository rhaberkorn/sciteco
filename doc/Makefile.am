# Make sure that $(SCITECO_FULL) and the SciTECO-based
# substitutor for *.in is available.
include $(top_srcdir)/bootstrap.am

# tedoc is a code documentation tool for SciTECO commands
# and macros, extracting comments from SciTECO and C
# code.
# It generates Troff manpage markup and acts as a Troff
# preprocessor to manpage templates.
dist_bin_SCRIPTS = tedoc.tes

%.in : %.template tedoc.tes @top_srcdir@/src/*.c
	$(SCITECO_FULL) -m -- @srcdir@/tedoc.tes \
	                -C $@ $< @top_srcdir@/src/*.c

# grosciteco is a troff postprocessor similar to grotty
# which can be used to produce SciTECO-friendly output
# (woman pages).
dist_bin_SCRIPTS += grosciteco.tes
# grosciteco, like many other postprocessors, works
# best with its own macro package.
# Unfortunately, there is no way to query the built-in
# Groff tmac path.
# Therefore, the "sciteco" macros are installed into
# SciTECO's scitecodatadir. Users can copy this file, set up
# environment variables like $GROFF_TMAC_PATH or pass
# the directory via `groff -M`.
dist_scitecodata_DATA = sciteco.tmac

# Fix up the hash-bang line of installed SciTECO scripts upon
# installation to refer to the installed sciteco binary.
# This takes --program-prefix into account.
#
# FIXME: This will patch the hash-bang line repeatedly.
install-exec-hook:
	$(SCITECO_FULL) -e "@EB'$(DESTDIR)$(bindir)/*.tes' 1U* \
	                    EJ-1<%*^[ 0,^Q::@FR'#!^EM^Xsciteco'#!$(SCITECO_INSTALLED)'> \
	                    :EX"

womendir = $(scitecolibdir)/women

women_DATA = grosciteco.tes.1.woman grosciteco.tes.1.woman.tec
CLEANFILES = grosciteco.tes.1.intermediate
EXTRA_DIST = grosciteco.tes.1.in

women_DATA += tedoc.tes.1.woman tedoc.tes.1.woman.tec
CLEANFILES += tedoc.tes.1.intermediate
EXTRA_DIST += tedoc.tes.1.in

women_DATA += sciteco.1.woman sciteco.1.woman.tec
CLEANFILES += sciteco.1.intermediate
EXTRA_DIST += sciteco.1.in

women_DATA += sciteco.7.woman sciteco.7.woman.tec
CLEANFILES += sciteco.7.intermediate sciteco.7.in sciteco.7.htbl
EXTRA_DIST += sciteco.7.template

women_DATA += tutorial.woman tutorial.woman.tec
CLEANFILES += tutorial.intermediate
EXTRA_DIST += tutorial.ms.in

CLEANFILES += $(women_DATA)

# NOTE: *.intermediate files are only generated since SciTECO scripts
# cannot currently read stdin, so the grosciteco postprocessor
# has to be run on a separate file.
%.woman %.woman.tec : %.intermediate grosciteco.tes
	$(SCITECO_FULL) -m -- @srcdir@/grosciteco.tes \
	                $@ $<

%.intermediate : % sciteco.tmac
	@GROFF@ -wall -Z -Kutf-8 -Tutf8 -t -man -M@srcdir@ -msciteco $< >$@

tutorial.intermediate : tutorial.ms sciteco.tmac
	@GROFF@ -wall -Z -Kutf-8 -Tutf8 -t -ms -M@srcdir@ -msciteco $< >$@

man_MANS = grosciteco.tes.1 tedoc.tes.1 sciteco.1 sciteco.7
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

SUFFIXES += .htbl .html
.htbl.html:
	@GROFF@ -wall -Thtml -man $< >$@

# FIXME: We may want to build the cheat sheet automatically.
# This would require a full Groff installation, though.
EXTRA_DIST += cheat-sheet.mm

#
# Doxygen processing (do not install or distribute)
#
devdoc : doxygen/

.PHONY: doxygen/
doxygen/ : Doxyfile
	-rm -rf doxygen/
	@DOXYGEN@ $<

clean-local:
	-rm -rf doxygen/
