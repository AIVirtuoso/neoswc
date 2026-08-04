/* neoswc: test/orphan_subsurface.c
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
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Crash the compositor by outliving your own window.
 *
 * A toplevel's compositor_view is destroyed when the window is finalized, and
 * compositor_view_destroy() sets surface->view to NULL. The wl_surface itself
 * belongs to the client and survives that -- so between destroying an
 * xdg_toplevel and destroying its wl_surface, a surface exists with no view
 * and, if it had any, a full list of subsurfaces still attached.
 *
 * Commit in that gap and swc walks the subsurfaces to update their visibility,
 * reads the *parent's* view, and dereferences NULL. That is what this program
 * does, in the smallest number of steps that reach it:
 *
 *   map a toplevel -> give it a subsurface -> destroy the toplevel ->
 *   commit the surface again
 *
 * Found from a core: a session died when Discord and OBS were closed in quick
 * succession, both of them clients that decorate themselves with subsurfaces.
 * foot does not reproduce it, because foot destroys its surface and its
 * toplevel together and never commits in between.
 *
 * Exit status is the test: 0 means the compositor survived the final commit,
 * and anything else means it did not (the roundtrip fails once the socket is
 * gone).
 */

#include "xdg-shell-client-protocol.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#define WIDTH 200
#define HEIGHT 150

static struct wl_compositor *compositor;
static struct wl_subcompositor *subcompositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static bool configured;

static void
handle_global(void *data, struct wl_registry *registry, uint32_t name,
              const char *interface, uint32_t version)
{
	(void)data;
	(void)version;

	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor =
		    wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
		subcompositor =
		    wl_registry_bind(registry, name, &wl_subcompositor_interface, 1);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
	}
}

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

static void
handle_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
	(void)data;
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = handle_ping,
};

static void
handle_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial)
{
	(void)data;
	xdg_surface_ack_configure(xdg_surface, serial);
	configured = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = handle_configure,
};

/* A solid buffer, because a subsurface with no buffer is never shown and the
 * visibility walk this is trying to reach would skip it. */
static struct wl_buffer *
make_buffer(uint32_t color)
{
	int stride = WIDTH * 4, size = stride * HEIGHT;
	struct wl_shm_pool *pool;
	struct wl_buffer *buffer;
	uint32_t *pixels;
	int fd, i;

	fd = memfd_create("orphan-subsurface", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0) {
		return NULL;
	}
	pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) {
		close(fd);
		return NULL;
	}
	for (i = 0; i < WIDTH * HEIGHT; ++i) {
		pixels[i] = color;
	}
	munmap(pixels, size);

	pool = wl_shm_create_pool(shm, fd, size);
	buffer = wl_shm_pool_create_buffer(pool, 0, WIDTH, HEIGHT, stride,
	                                   WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	return buffer;
}

int
main(void)
{
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_surface *parent, *child;
	struct wl_subsurface *subsurface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *toplevel;
	struct wl_buffer *parent_buffer, *child_buffer;
	int i;

	display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "orphan: cannot connect to the display\n");
		return 1;
	}

	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);

	if (!compositor || !subcompositor || !shm || !wm_base) {
		fprintf(stderr, "orphan: missing globals\n");
		return 1;
	}
	xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);

	/* 1. Map a toplevel. */
	parent = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, parent);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_set_title(toplevel, "orphan-subsurface");
	wl_surface_commit(parent);

	for (i = 0; i < 20 && !configured; ++i) {
		if (wl_display_roundtrip(display) < 0) {
			fprintf(stderr, "orphan: display error awaiting configure\n");
			return 1;
		}
	}
	if (!configured) {
		fprintf(stderr, "orphan: never configured\n");
		return 1;
	}

	parent_buffer = make_buffer(0xff2020a0);
	if (!parent_buffer) {
		fprintf(stderr, "orphan: cannot allocate\n");
		return 1;
	}
	wl_surface_attach(parent, parent_buffer, 0, 0);
	wl_surface_damage(parent, 0, 0, WIDTH, HEIGHT);
	wl_surface_commit(parent);
	wl_display_roundtrip(display);

	/* 2. Give it a subsurface, the way a client-side titlebar would. */
	child = wl_compositor_create_surface(compositor);
	subsurface = wl_subcompositor_get_subsurface(subcompositor, child, parent);
	wl_subsurface_set_desync(subsurface);
	wl_subsurface_set_position(subsurface, 10, 10);

	child_buffer = make_buffer(0xff20a020);
	if (!child_buffer) {
		fprintf(stderr, "orphan: cannot allocate\n");
		return 1;
	}
	wl_surface_attach(child, child_buffer, 0, 0);
	wl_surface_damage(child, 0, 0, WIDTH, HEIGHT);
	wl_surface_commit(child);
	wl_surface_commit(parent);
	wl_display_roundtrip(display);

	/*
	 * 3. Destroy the window but keep the surface. This is the step that makes
	 * the compositor finalize the window and null out surface->view, while the
	 * subsurface stays attached to a wl_surface that is still very much alive.
	 */
	xdg_toplevel_destroy(toplevel);
	xdg_surface_destroy(xdg_surface);
	if (wl_display_roundtrip(display) < 0) {
		fprintf(stderr, "orphan: display error destroying the toplevel\n");
		return 1;
	}

	/* 4. Commit into the gap. */
	wl_surface_commit(parent);
	if (wl_display_roundtrip(display) < 0) {
		fprintf(stderr, "orphan: FAILED -- compositor died on the commit\n");
		return 1;
	}

	printf("orphan: OK -- compositor survived the orphaned commit\n");
	wl_display_disconnect(display);

	return 0;
}
