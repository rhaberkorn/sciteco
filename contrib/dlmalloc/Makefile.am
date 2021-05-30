# Source: http://gee.cs.oswego.edu/dl/html/malloc.html
#
# FIXME: On FreeBSD, we might implement a compatible mremap() based on the BSD mremap() and pass
# in -DHAVE_MREMAP=1 -DMREMAP=mremap_bsd. We'll have to add a declaration to malloc.c or
# use -include mremap_bsd.h in CPPFLAGS.
#
# FIXME: It may be advisable to add malloc.c directly to the programs' SOURCES variable
# for increased portability. There is also AC_LIBOBJ, but it's usually for defining sources of
# replacement libraries.

AM_CPPFLAGS = -DINSECURE='defined(NDEBUG)' -DNO_MALLINFO=1 -DNO_MALLOC_STATS=1 \
              -DUSE_LOCKS=1 -DUSE_DL_PREFIX

# FIXME: This optimization is still broken as of GCC v9.3.0.
# This is a known GCC bug, triggered by memset() in calloc().
# See https://gcc.gnu.org/bugzilla/show_bug.cgi?id=93213
# FIXME: This must not be passed on Clang
AM_CFLAGS = -fno-optimize-strlen

noinst_LTLIBRARIES = libdlmalloc.la
if REPLACE_MALLOC
libdlmalloc_la_SOURCES = malloc.c malloc.h
endif
