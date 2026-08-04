/* swc: libswc/image_capture.h
 *
 * Copyright (c) 2026 neoswc contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef SWC_IMAGE_CAPTURE_H
#define SWC_IMAGE_CAPTURE_H

#include <stdbool.h>

struct wl_display;
struct wl_global;

/*
 * ext-image-copy-capture-v1 plus the output source manager it needs. Two
 * globals, created together because a client is useless with only one of them.
 */
bool
image_capture_create(struct wl_display *display, struct wl_global **manager,
                     struct wl_global **output_source);

#endif
