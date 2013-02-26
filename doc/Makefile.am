include $(top_srcdir)/bootstrap.am

man_MANS = sciteco.1
EXTRA_DIST = sciteco.1.in
CLEANFILES = sciteco.1

man_MANS += sciteco.7
EXTRA_DIST += sciteco.7.template
CLEANFILES += sciteco.7 sciteco.7.in
SOURCES = $(wildcard @top_srcdir@/src/*.cpp)

EXTRA_DIST += generate-docs.tes

%.in : %.template generate-docs.tes $(SOURCES)
	$(BOOTSTRAP_SCITECO) -m @srcdir@/generate-docs.tes \
			     $@ $< $(SOURCES)

#
# Doxygen processing (do not install or distribute)
#
if BUILD_DEVDOC
noinst_DATA = doxygen/
endif

.PHONY: doxygen/
doxygen/ : Doxyfile
	@DOXYGEN@ $<

clean-local:
	-rm -rf doxygen/
