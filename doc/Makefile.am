include $(top_srcdir)/bootstrap.am

man_MANS = sciteco.1
EXTRA_DIST = sciteco.1.in
CLEANFILES = sciteco.1

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
