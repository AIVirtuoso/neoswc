/* neoswc: test/cursor_orphan.c
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
 * Take the mouse pointer away and leave.
 *
 * Two ways a client can end up owning the cursor and then abandoning it, both
 * of which left swc with no pointer at all until some other client happened to
 * set one:
 *
 *   destroy   set a cursor surface, then destroy it. swc attached NULL to
 *             every screen's cursor plane and stopped there.
 *   hide      set_cursor(NULL), which the protocol defines as hiding the
 *             pointer. swc kept showing the previous image instead, and
 *             nothing restored the compositor's cursor on the way out.
 *
 * Pass "destroy" or "hide" as the mode. The program then stays alive so the
 * DRM cursor plane can be inspected from outside while its window still has
 * the pointer -- the compositor draws the cursor on its own plane, which never
 * reaches the shadow buffer and so never appears in a screendump.
 *
 * Exiting is itself part of the test: once this client is gone the pointer
 * enters another window, and the cursor must come back.
 */

#include "xdg-shell-client-protocol.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#define WIDTH 400
#define HEIGHT 300
#define CURSOR_SIZE 24

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct xdg_wm_base *wm_base;
static struct wl_pointer *pointer;
static bool configured;
static bool entered;
static uint32_t enter_serial;

static void
handle_global(void *data, struct wl_registry *registry, uint32_t name,
              const char *interface, uint32_t version)
{
	(void)data;
	(void)version;

	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor =
		    wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 5);
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

static void
pointer_enter(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
              struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y)
{
	(void)data;
	(void)wl_pointer;
	(void)surface;
	(void)x;
	(void)y;

	enter_serial = serial;
	entered = true;
}

static void
pointer_noop_leave(void *data, struct wl_pointer *p, uint32_t serial,
                   struct wl_surface *surface)
{
	(void)data;
	(void)p;
	(void)serial;
	(void)surface;
}

static void
pointer_noop_motion(void *data, struct wl_pointer *p, uint32_t time,
                    wl_fixed_t x, wl_fixed_t y)
{
	(void)data;
	(void)p;
	(void)time;
	(void)x;
	(void)y;
}

static void
pointer_noop_button(void *data, struct wl_pointer *p, uint32_t serial,
                    uint32_t time, uint32_t button, uint32_t state)
{
	(void)data;
	(void)p;
	(void)serial;
	(void)time;
	(void)button;
	(void)state;
}

static void
pointer_noop_axis(void *data, struct wl_pointer *p, uint32_t time,
                  uint32_t axis, wl_fixed_t value)
{
	(void)data;
	(void)p;
	(void)time;
	(void)axis;
	(void)value;
}

static void
pointer_noop(void *data, struct wl_pointer *p)
{
	(void)data;
	(void)p;
}

static void
pointer_noop_u32(void *data, struct wl_pointer *p, uint32_t v)
{
	(void)data;
	(void)p;
	(void)v;
}

static void
pointer_noop_axis_stop(void *data, struct wl_pointer *p, uint32_t time,
                       uint32_t axis)
{
	(void)data;
	(void)p;
	(void)time;
	(void)axis;
}

static void
pointer_noop_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis,
                           int32_t discrete)
{
	(void)data;
	(void)p;
	(void)axis;
	(void)discrete;
}

static void
pointer_noop_axis_value120(void *data, struct wl_pointer *p, uint32_t axis,
                           int32_t value120)
{
	(void)data;
	(void)p;
	(void)axis;
	(void)value120;
}

static void
pointer_noop_axis_relative_direction(void *data, struct wl_pointer *p,
                                     uint32_t axis, uint32_t direction)
{
	(void)data;
	(void)p;
	(void)axis;
	(void)direction;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_noop_leave,
    .motion = pointer_noop_motion,
    .button = pointer_noop_button,
    .axis = pointer_noop_axis,
    .frame = pointer_noop,
    .axis_source = pointer_noop_u32,
    .axis_stop = pointer_noop_axis_stop,
    .axis_discrete = pointer_noop_axis_discrete,
    .axis_value120 = pointer_noop_axis_value120,
    .axis_relative_direction = pointer_noop_axis_relative_direction,
};

static struct wl_buffer *
make_buffer(int width, int height, uint32_t color)
{
	int stride = width * 4, size = stride * height;
	struct wl_shm_pool *pool;
	struct wl_buffer *buffer;
	uint32_t *pixels;
	int fd, i;

	fd = memfd_create("cursor-orphan", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0) {
		return NULL;
	}
	pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) {
		close(fd);
		return NULL;
	}
	for (i = 0; i < width * height; ++i) {
		pixels[i] = color;
	}
	munmap(pixels, size);

	pool = wl_shm_create_pool(shm, fd, size);
	buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
	                                   WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	return buffer;
}

int
main(int argc, char *argv[])
{
	const char *mode = argc > 1 ? argv[1] : "destroy";
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_surface *surface, *cursor_surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *toplevel;
	struct wl_buffer *buffer, *cursor_buffer;
	int i;

	/* stdout is a file here and this program lives for a while after it has
	 * something to say, so block buffering would hold every line until exit --
	 * and the run samples the log before then. */
	setvbuf(stdout, NULL, _IOLBF, 0);

	display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "cursor: cannot connect to the display\n");
		return 1;
	}

	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);

	if (!compositor || !shm || !seat || !wm_base) {
		fprintf(stderr, "cursor: missing globals\n");
		return 1;
	}
	xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
	pointer = wl_seat_get_pointer(seat);
	wl_pointer_add_listener(pointer, &pointer_listener, NULL);

	surface = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_set_title(toplevel, "cursor-orphan");
	xdg_toplevel_set_app_id(toplevel, "cursor-orphan");
	wl_surface_commit(surface);

	for (i = 0; i < 20 && !configured; ++i) {
		if (wl_display_roundtrip(display) < 0) {
			fprintf(stderr, "cursor: display error awaiting configure\n");
			return 1;
		}
	}
	if (!(buffer = make_buffer(WIDTH, HEIGHT, 0xff303030))) {
		fprintf(stderr, "cursor: cannot allocate\n");
		return 1;
	}
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage(surface, 0, 0, WIDTH, HEIGHT);
	wl_surface_commit(surface);
	wl_display_roundtrip(display);

	/* The pointer has to be over this window for set_cursor to be honoured;
	 * swc drops the request otherwise. Give the manager a few rounds to place
	 * the window under it. */
	for (i = 0; i < 200 && !entered; ++i) {
		if (wl_display_roundtrip(display) < 0) {
			fprintf(stderr, "cursor: display error awaiting enter\n");
			return 1;
		}
		usleep(100000);
	}
	if (!entered) {
		printf("cursor: SKIPPED -- the pointer never entered this window\n");
		return 2;
	}

	if (strcmp(mode, "hide") == 0) {
		wl_pointer_set_cursor(pointer, enter_serial, NULL, 0, 0);
		printf("cursor: hid the pointer with a NULL surface\n");
	} else {
		cursor_surface = wl_compositor_create_surface(compositor);
		cursor_buffer = make_buffer(CURSOR_SIZE, CURSOR_SIZE, 0xff00ff00);
		if (!cursor_buffer) {
			fprintf(stderr, "cursor: cannot allocate\n");
			return 1;
		}
		wl_surface_attach(cursor_surface, cursor_buffer, 0, 0);
		wl_surface_damage(cursor_surface, 0, 0, CURSOR_SIZE, CURSOR_SIZE);
		wl_surface_commit(cursor_surface);
		wl_pointer_set_cursor(pointer, enter_serial, cursor_surface, 0, 0);
		wl_display_roundtrip(display);

		/*
		 * Whether the surface still has unconsumed damage when it dies decides
		 * everything, because swc's cursor attach() returns early for a
		 * damage-free surface and so never detaches the plane:
		 *
		 *   destroy        no damage pending -- the stale image stays on screen
		 *   destroy-dirty  damage pending -- the plane is cleared and the user
		 *                  is left with no pointer at all
		 *
		 * Chromium recycles cursor surfaces without waiting for a frame, which
		 * is the second one.
		 */
		if (strcmp(mode, "destroy-dirty") == 0) {
			wl_surface_attach(cursor_surface, cursor_buffer, 0, 0);
			wl_surface_damage(cursor_surface, 0, 0, CURSOR_SIZE, CURSOR_SIZE);
			wl_surface_commit(cursor_surface);
		}

		/* ...and then abandon it, which is the whole point. */
		wl_surface_destroy(cursor_surface);
		printf("cursor: destroyed its own cursor surface (%s)\n", mode);
	}

	if (wl_display_roundtrip(display) < 0) {
		fprintf(stderr, "cursor: FAILED -- compositor died\n");
		return 1;
	}

	/* Hold the window so the cursor plane can be read from outside. */
	for (i = 0; i < 60; ++i) {
		if (wl_display_roundtrip(display) < 0) {
			break;
		}
		usleep(100000);
	}

	wl_display_disconnect(display);

	return 0;
}
