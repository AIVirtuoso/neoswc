# swc: protocol/local.mk

dir := protocol
wayland_protocols := $(call pkgconfig,wayland-protocols,variable=pkgdatadir,DATADIR)

PROTOCOL_EXTENSIONS =           \
    $(dir)/server-decoration.xml\
    $(dir)/swc.xml              \
    $(dir)/swc_snap.xml         \
    $(dir)/wayland-drm.xml      \
    $(wayland_protocols)/stable/xdg-shell/xdg-shell.xml \
    $(wayland_protocols)/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml \
    $(wayland_protocols)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml

$(dir)_PACKAGES := wayland-server

.for ext in ${PROTOCOL_EXTENSIONS}
proto_base := ${ext:T:R}

${dir}/${proto_base}-protocol.c: ${ext} \
    ${dir}/${proto_base}-server-protocol.h \
    ${dir}/${proto_base}-client-protocol.h
	${Q_GEN}${WAYLAND_SCANNER} code ${ext} ${.TARGET}

${dir}/${proto_base}-server-protocol.h: ${ext}
	${Q_GEN}${WAYLAND_SCANNER} server-header ${ext} ${.TARGET}

${dir}/${proto_base}-client-protocol.h: ${ext}
	${Q_GEN}${WAYLAND_SCANNER} client-header ${ext} ${.TARGET}

CLEAN_FILES += ${dir}/${proto_base}-protocol.c \
    ${dir}/${proto_base}-server-protocol.h \
    ${dir}/${proto_base}-client-protocol.h
.endfor

install-$(dir): | $(DESTDIR)$(DATADIR)/swc
	install -m 644 protocol/swc.xml $(DESTDIR)$(DATADIR)/swc

include common.mk
