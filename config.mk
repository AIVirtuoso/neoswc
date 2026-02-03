# swc: config.mk

# The commented out options are defaults

PREFIX          = /usr
# BINDIR          = $(PREFIX)/bin
# LIBDIR          = $(PREFIX)/lib
# INCLUDEDIR      = $(PREFIX)/include
# DATADIR         = $(PREFIX)/share
# PKGCONFIGDIR    = $(LIBDIR)/pkgconfig

# OBJCOPY         = objcopy
# PKG_CONFIG      = pkg-config
# WAYLAND_SCANNER = wayland-scanner

ENABLE_DEBUG    = 1
ENABLE_STATIC   = 1
ENABLE_SHARED   = 0
ENABLE_LIBUDEV  = 1
ENABLE_XWAYLAND = 1

#INPUT_BACKEND  = libinput
#   available: libinput, evdev, wscons
#   default: libinput on linux, wscons on NetBSD
