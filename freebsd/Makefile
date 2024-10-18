PORTNAME=	sciteco
DISTVERSION=	2.1.0
CATEGORIES=	editors textproc devel
MASTER_SITES=	https://github.com/rhaberkorn/${PORTNAME}/releases/download/v${DISTVERSION}/ \
		SOURCEFORGE/${PORTNAME}/v${DISTVERSION}/

MAINTAINER=	robin.haberkorn@googlemail.com
COMMENT=	Scintilla-based Text Editor and Corrector
WWW=		https://rhaberkorn.github.io/sciteco/

LICENSE=	GPLv3+
LICENSE_FILE=	${WRKSRC}/COPYING

FLAVORS=	curses gtk
FLAVOR?=	${FLAVORS:[1]}
curses_PKGNAMESUFFIX=-curses
gtk_PKGNAMESUFFIX=-gtk

# As SciTECO uses itself during the build process,
# it makes sense to compile it running under a dummy XServer.
# This is both faster and works in headless environments as well.
gtk_BUILD_DEPENDS=Xvfb:x11-servers/xorg-server@xvfb \
		  xauth:x11/xauth mcookie:devel/util-linux

USES=		gmake pkgconfig compiler:c11 compiler:c++17-lang gnome groff
USE_GNOME=	glib20

GNU_CONFIGURE=	yes
CONFIGURE_ARGS=
CONFIGURE_OUTSOURCE=yes

MAKEFILE=	GNUmakefile
TEST_TARGET=	check

# NOTE: Unlike on Debian, we cannot build a sciteco-common package.
# FreeBSD does not yet support subpackages.
# Therefore both flavors will install totally independant
# (partially redundant) files.
.if ${FLAVOR} == gtk
DATADIR=	${PREFIX}/share/gsciteco
.endif

SUB_FILES=	pkg-message

.if ${FLAVOR} == curses
USES+=		ncurses
CONFIGURE_ARGS+=--with-interface=ncurses
PLIST_SUB+=	GTK="@comment " PROGRAM_PREFIX=""
.elif ${FLAVOR} == gtk
USES+=		desktop-file-utils
# FIXME: To appease QA checks, we would have to
# USE_GNOME+=cairo gdkpixbuf2 and
# gtk_LIB_DEPENDS+=libharfbuzz.so:print/harfbuzz
USE_GNOME+=	gtk30
CONFIGURE_ARGS+=--with-interface=gtk --program-prefix=g \
		--with-scitecodatadir="${DATADIR}"
PLIST_SUB+=	GTK="" PROGRAM_PREFIX=g
.endif

OPTIONS_DEFINE=	LEXILLA MALLOC_REPLACEMENT TECO_INTEGER_32
OPTIONS_DEFAULT=LEXILLA
OPTIONS_SUB=	yes

LEXILLA_DESC=Build with Lexilla lexer support (larger)
MALLOC_REPLACEMENT_DESC=Force replacement of system malloc()
TECO_INTEGER_32_DESC=Use 32-bit TECO integers

LEXILLA_CONFIGURE_OFF=--without-lexilla
MALLOC_REPLACEMENT_CONFIGURE_ON=--enable-malloc-replacement
TECO_INTEGER_32_CONFIGURE_ON=--with-teco-integer=32

.include <bsd.port.pre.mk>

.if ${FLAVOR} == gtk
# Run under Xvfb (see above).
# This is done only now, since USES=gmake would overwrite MAKE_CMD.
MAKE_CMD="${SCRIPTDIR}/xvfb-run.sh" ${GMAKE}
.endif

post-install:
.for SZ in 16 32 48 256
	${MKDIR} ${STAGEDIR}${PREFIX}/share/icons/hicolor/${SZ}x${SZ}/apps
	${INSTALL_DATA} ${WRKSRC}/ico/sciteco-${SZ}.png \
			${STAGEDIR}${PREFIX}/share/icons/hicolor/${SZ}x${SZ}/apps/sciteco.png
.endfor
	# To appease `make check-plist`:
	${RM} ${STAGEDIR}${DATADIR}/*.png
	${MKDIR} ${STAGEDIR}${PREFIX}/share/applications
	${INSTALL_DATA} ${WRKSRC}/src/sciteco.desktop \
			${STAGEDIR}${PREFIX}/share/applications/sciteco.desktop

.include <bsd.port.post.mk>
