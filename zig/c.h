/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 neoswc contributors
 *
 * Single translation unit for the Zig bindings.
 *
 * swc.h only forward-declares struct wl_display, so translating it alone would
 * produce an opaque type that cannot be created or run. Pulling in
 * wayland-server.h first gives the Zig side the whole surface it needs from
 * one module.
 */

#include <stdlib.h>
#include <wayland-server.h>

#include <swc.h>
