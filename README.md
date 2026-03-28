neuswc
------

neuswc is a fork of [swc](https://github.com/michaelforney/swc/) for [hevel window manager](hevel.derivelinux.org).

It introduces bunch of new features.

features
--------

- z axis ordering
- more cursor functions
- zooming
- experimental subsurface support
- fullscreen
- double window borders
- wallpapers
- screenshots
- evdev-only input backend
- probably more i forgot about

build
-----

you will need: 
- A C99-compatible compiler
- BSD make
- pkg-config
- wayland-scanner, wayland-server, wayland-client
- wayland-server, wayland-client
- libdrm, pixman, xkbcommon
- neuwld
- on linux, either libudev or evdev, depending on your choice of input backend, neither on BSD
- xcb, xcb-composite, xcb-ewmh and xcb-icccm if you want Xwayland support

then, run 

meson setup build
ninja -C build
meson -C build install
