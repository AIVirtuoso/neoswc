/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 neoswc contributors
 */

#ifndef NEOSWC_RIVER_WM_H
#define NEOSWC_RIVER_WM_H

#include <stdbool.h>

struct wl_display;
struct swc_screen;
struct swc_window;

bool
river_wm_create(struct wl_display *display);

void
river_wm_add_screen(struct swc_screen *screen);
void
river_wm_add_window(struct swc_window *window);

#endif
