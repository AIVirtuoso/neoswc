# bmake-compatible build for swc (avoids GNU make-specific functions/macros).

.include "config.mk"

.MAIN: build

.if !defined(PREFIX)
PREFIX=/usr/local
.endif
.if !defined(BINDIR)
BINDIR=${PREFIX}/bin
.endif
.if !defined(LIBDIR)
LIBDIR=${PREFIX}/lib
.endif
.if !defined(INCLUDEDIR)
INCLUDEDIR=${PREFIX}/include
.endif
.if !defined(DATADIR)
DATADIR=${PREFIX}/share
.endif
.if !defined(PKGCONFIGDIR)
PKGCONFIGDIR=${LIBDIR}/pkgconfig
.endif

.if !defined(OBJCOPY)
OBJCOPY=objcopy
.endif
.if !defined(PKG_CONFIG)
PKG_CONFIG=pkg-config
.endif
.if !defined(WAYLAND_SCANNER)
WAYLAND_SCANNER=wayland-scanner
.endif
.if !defined(CC)
CC=cc
.endif
.if !defined(AR)
AR=ar
.endif

UNAME!= uname

VERSION_MAJOR=0
VERSION_MINOR=0
VERSION=${VERSION_MAJOR}.${VERSION_MINOR}

PACKAGES=libdrm pixman-1 wayland-server wayland-protocols wld xkbcommon

.if ${UNAME} != "NetBSD"
PACKAGES+= libinput
.if defined(ENABLE_LIBUDEV) && ${ENABLE_LIBUDEV} == 1
PACKAGES+= libudev
.endif
.endif

.if defined(ENABLE_XWAYLAND) && ${ENABLE_XWAYLAND} == 1
PACKAGES+= xcb xcb-composite xcb-ewmh xcb-icccm
.endif

PKG_CFLAGS!= ${PKG_CONFIG} --cflags ${PACKAGES}
PKG_LIBS!= ${PKG_CONFIG} --libs ${PACKAGES}
WAYLAND_PROTOCOLS_DATADIR!= ${PKG_CONFIG} --variable=pkgdatadir wayland-protocols

CPPFLAGS+= -D_GNU_SOURCE
CFLAGS+= -fvisibility=hidden -std=c11 ${PKG_CFLAGS}
CFLAGS+= -Werror=implicit-function-declaration -Werror=implicit-int -Werror=pointer-sign -Werror=pointer-arith -Wall -Wno-missing-braces

.if defined(ENABLE_DEBUG) && ${ENABLE_DEBUG} == 1
CPPFLAGS+= -DENABLE_DEBUG=1
CFLAGS+= -g
.endif

.if defined(ENABLE_LIBUDEV) && ${ENABLE_LIBUDEV} == 1
CPPFLAGS+= -DENABLE_LIBUDEV
.endif
.if defined(ENABLE_XWAYLAND) && ${ENABLE_XWAYLAND} == 1
CPPFLAGS+= -DENABLE_XWAYLAND
.endif

PROTO_EXTENSIONS= \
	protocol/server-decoration.xml \
	protocol/swc.xml \
	protocol/swc_snap.xml \
	protocol/wayland-drm.xml \
	${WAYLAND_PROTOCOLS_DATADIR}/stable/xdg-shell/xdg-shell.xml \
	${WAYLAND_PROTOCOLS_DATADIR}/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml \
	${WAYLAND_PROTOCOLS_DATADIR}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml

.for xml in ${PROTO_EXTENSIONS}
_base=${xml:T:R}
protocol/${_base}-protocol.c: ${xml}
	@echo "  GEN $@"
	@${WAYLAND_SCANNER} code < ${.ALLSRC} > ${.TARGET}

protocol/${_base}-server-protocol.h: ${xml}
	@echo "  GEN $@"
	@${WAYLAND_SCANNER} server-header < ${.ALLSRC} > ${.TARGET}
.endfor

PROTO_GEN_C= \
	protocol/linux-dmabuf-unstable-v1-protocol.c \
	protocol/server-decoration-protocol.c \
	protocol/swc-protocol.c \
	protocol/swc_snap-protocol.c \
	protocol/wayland-drm-protocol.c \
	protocol/xdg-decoration-unstable-v1-protocol.c \
	protocol/xdg-shell-protocol.c

PROTO_GEN_H= \
	protocol/linux-dmabuf-unstable-v1-server-protocol.h \
	protocol/server-decoration-server-protocol.h \
	protocol/swc-server-protocol.h \
	protocol/swc_snap-server-protocol.h \
	protocol/wayland-drm-server-protocol.h \
	protocol/xdg-decoration-unstable-v1-server-protocol.h \
	protocol/xdg-shell-server-protocol.h

cursor/cursor_data.h: cursor/cursor.pcf cursor/convert_font
	@echo "  GEN $@"
	@cursor/convert_font cursor/cursor.pcf ${.TARGET} 2>/dev/null

cursor/convert_font: cursor/convert_font.o
	@echo "  CCLD $@"
	@${CC} ${LDFLAGS} -o ${.TARGET} ${.ALLSRC}

cursor/convert_font.o: cursor/convert_font.c
	@echo "  CC $@"
	@${CC} ${CPPFLAGS} ${CFLAGS} -I . -c -o ${.TARGET} ${.ALLSRC}

LAUNCH_DEVMAJOR=launch/devmajor-linux.c
.if ${UNAME} == "NetBSD"
LAUNCH_DEVMAJOR=launch/devmajor-netbsd.c
.endif

launch/swc-launch: launch/launch.o launch/protocol.o launch/${LAUNCH_DEVMAJOR:T:R}.o
	@echo "  CCLD $@"
	@${CC} ${LDFLAGS} -o ${.TARGET} ${.ALLSRC} ${PKG_LIBS}

.for f in launch/launch.c launch/protocol.c ${LAUNCH_DEVMAJOR}
launch/${f:T:R}.o: ${f}
	@echo "  CC $@"
	@${CC} ${CPPFLAGS} ${CFLAGS} -I . -c -o ${.TARGET} ${.ALLSRC}
.endfor

SWC_SOURCES= \
	libswc/bindings.c \
	libswc/compositor.c \
	libswc/data.c \
	libswc/data_device.c \
	libswc/data_device_manager.c \
	libswc/dmabuf.c \
	libswc/drm.c \
	libswc/input.c \
	libswc/kde_decoration.c \
	libswc/keyboard.c \
	libswc/launch.c \
	libswc/mode.c \
	libswc/output.c \
	libswc/panel.c \
	libswc/panel_manager.c \
	libswc/plane.c \
	libswc/pointer.c \
	libswc/primary_plane.c \
	libswc/region.c \
	libswc/screen.c \
	libswc/shell.c \
	libswc/shell_surface.c \
	libswc/shm.c \
	libswc/snap.c \
	libswc/subcompositor.c \
	libswc/subsurface.c \
	libswc/surface.c \
	libswc/swc.c \
	libswc/util.c \
	libswc/view.c \
	libswc/wallpaper.c \
	libswc/wayland_buffer.c \
	libswc/window.c \
	libswc/xdg_decoration.c \
	libswc/xdg_shell.c \
	protocol/linux-dmabuf-unstable-v1-protocol.c \
	protocol/server-decoration-protocol.c \
	protocol/swc-protocol.c \
	protocol/swc_snap-protocol.c \
	protocol/wayland-drm-protocol.c \
	protocol/xdg-decoration-unstable-v1-protocol.c \
	protocol/xdg-shell-protocol.c

.if ${UNAME} == "NetBSD"
SWC_SOURCES+= libswc/seat-ws.c
.else
SWC_SOURCES+= libswc/seat.c
.endif

.if defined(ENABLE_XWAYLAND) && ${ENABLE_XWAYLAND} == 1
SWC_SOURCES+= libswc/xserver.c libswc/xwm.c
.endif

SWC_OBJECTS=${SWC_SOURCES:S/.c/.o/}
SWC_OBJECTS+= launch/protocol.o

.for src in ${SWC_SOURCES}
${src:R}.o: ${src} ${PROTO_GEN_H} cursor/cursor_data.h
	@echo "  CC $@"
	@${CC} ${CPPFLAGS} ${CFLAGS} -I . -I protocol -c -o ${.TARGET} ${.IMPSRC}
.endfor

libswc/libswc-internal.o: ${SWC_OBJECTS}
	@echo "  CCLD $@"
	@${CC} -nostdlib -r -o ${.TARGET} ${.ALLSRC}

libswc/libswc.o: libswc/libswc-internal.o
	@echo "  OBJCOPY $@"
	@${OBJCOPY} --localize-hidden ${.ALLSRC} ${.TARGET}

libswc/libswc.a: libswc/libswc.o
	@echo "  AR $@"
	@${AR} cru ${.TARGET} ${.ALLSRC}

swc.pc: swc.pc.in
	@echo "  GEN $@"
	@sed -e 's:@VERSION@:${VERSION}:' \
	     -e 's:@PREFIX@:${PREFIX}:' \
	     -e 's:@LIBDIR@:${LIBDIR}:' \
	     -e 's:@INCLUDEDIR@:${INCLUDEDIR}:' \
	     -e 's:@DATADIR@:${DATADIR}:' \
	     -e 's:@REQUIRES@:wayland-server:' \
	     -e 's:@REQUIRES_PRIVATE@::' \
	     ${.ALLSRC} > ${.TARGET}

.PHONY: all build clean
all: build
build: libswc/libswc.a launch/swc-launch cursor/cursor_data.h swc.pc

clean:
	rm -f swc.pc \
	      ${PROTO_GEN_C} ${PROTO_GEN_H} \
	      cursor/cursor_data.h cursor/convert_font cursor/convert_font.o \
	      launch/*.o launch/swc-launch \
	      libswc/*.o libswc/libswc-internal.o libswc/libswc.o libswc/libswc.a \
	      protocol/*.o
