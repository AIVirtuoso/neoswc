/* swc: libswc/output.h
 *
 * Modifications copyright (c) 2026 neoswc contributors
 *
 * SPDX-License-Identifier: MIT AND GPL-3.0-or-later
 *
 * The upstream file carries no header of its own; it is covered by the MIT
 * LICENSE at the top of the tree. Modifications by neoswc contributors are
 * licensed GPL-3.0-or-later; see COPYING.
 */

#ifndef SWC_OUTPUT_H
#define SWC_OUTPUT_H

#include <pixman.h>
#include <stdint.h>
#include <wayland-util.h>
#include <xf86drmMode.h>

struct wl_display;

struct output {
	struct wl_resource *resource;
	struct screen *screen;

	char name[24];
	/* The physical dimensions (in mm) of this output */
	uint32_t physical_width, physical_height;

	struct wl_array modes;
	struct mode *preferred_mode;

	pixman_region32_t current_damage, previous_damage;

	/* The DRM connector corresponding to this output */
	uint32_t connector;

	struct wl_global *global;
	struct wl_list resources;
	struct wl_list link;
};

/*
 * Re-send wl_output.geometry to everyone already bound. Only needed when the
 * screen moves; the bind path sends it once and nothing else changed it,
 * because until swc_screen_set_position() a screen could not move.
 */
void
output_send_geometry(struct output *output);

struct output *
output_new(drmModeConnector *connector);
void
output_destroy(struct output *output);

#endif
