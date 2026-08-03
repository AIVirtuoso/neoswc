/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 neoswc contributors
 *
 * A minimal river-window-management-v1 client, to drive the compositor's
 * protocol implementation.
 *
 * It tiles windows in a column: proposes dimensions during the manage
 * sequence, positions the nodes during the render sequence, and reports each
 * step so a run can be judged from the log. Deliberately small -- this is a
 * test for the server side, not a window manager anyone should use.
 */

#include "river-window-management-v1-client-protocol.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

#define MAX_WINDOWS 16

struct window {
	struct river_window_v1 *proxy;
	struct river_node_v1 *node;
	int32_t width, height;
	bool closed;
};

static struct river_window_manager_v1 *manager;
static struct window windows[MAX_WINDOWS];
static unsigned num_windows;
static bool running = true;

/* Learned from river_output_v1; these are only a fallback for the window that
 * arrives before any output has been advertised. */
static int32_t screen_width = 1280;
static int32_t screen_height = 800;

static void
say(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	fputs("wmclient: ", stderr);
	vfprintf(stderr, fmt, args);
	fputc('\n', stderr);
	va_end(args);
	fflush(stderr);
}

/* ---------------------------------------------------------------- window */

static void
window_closed(void *data, struct river_window_v1 *proxy)
{
	struct window *window = data;

	(void)proxy;
	window->closed = true;
	say("window closed");
}

static void
window_dimensions(void *data, struct river_window_v1 *proxy, int32_t width,
                  int32_t height)
{
	struct window *window = data;

	(void)proxy;
	window->width = width;
	window->height = height;
	say("window dimensions %dx%d", width, height);
}

static void
window_title(void *data, struct river_window_v1 *proxy, const char *title)
{
	(void)data;
	(void)proxy;
	say("window title %s", title ? title : "(null)");
}

static void
window_app_id(void *data, struct river_window_v1 *proxy, const char *app_id)
{
	(void)data;
	(void)proxy;
	say("window app_id %s", app_id ? app_id : "(null)");
}

static void
window_dimensions_hint(void *data, struct river_window_v1 *proxy,
                       int32_t min_width, int32_t min_height,
                       int32_t max_width, int32_t max_height)
{
	(void)data;
	(void)proxy;
	(void)min_width;
	(void)min_height;
	(void)max_width;
	(void)max_height;
}

static void
window_parent(void *data, struct river_window_v1 *proxy,
              struct river_window_v1 *parent)
{
	(void)data;
	(void)proxy;
	(void)parent;
}

static void
window_u32(void *data, struct river_window_v1 *proxy, uint32_t value)
{
	(void)data;
	(void)proxy;
	(void)value;
}

static void
window_none(void *data, struct river_window_v1 *proxy)
{
	(void)data;
	(void)proxy;
}

static void
window_xy(void *data, struct river_window_v1 *proxy, int32_t x, int32_t y)
{
	(void)data;
	(void)proxy;
	(void)x;
	(void)y;
}

static void
window_move_requested(void *data, struct river_window_v1 *proxy,
                      struct river_seat_v1 *seat)
{
	(void)data;
	(void)proxy;
	(void)seat;
}

static void
window_resize_requested(void *data, struct river_window_v1 *proxy,
                        struct river_seat_v1 *seat, uint32_t edges)
{
	(void)data;
	(void)proxy;
	(void)seat;
	(void)edges;
}

static void
window_fullscreen_requested(void *data, struct river_window_v1 *proxy,
                            struct river_output_v1 *output)
{
	(void)data;
	(void)proxy;
	(void)output;
}

static void
window_i32(void *data, struct river_window_v1 *proxy, int32_t value)
{
	(void)data;
	(void)proxy;
	(void)value;
}

static void
window_str(void *data, struct river_window_v1 *proxy, const char *value)
{
	(void)data;
	(void)proxy;
	(void)value;
}

static const struct river_window_v1_listener window_listener = {
    .closed = window_closed,
    .dimensions_hint = window_dimensions_hint,
    .dimensions = window_dimensions,
    .app_id = window_app_id,
    .title = window_title,
    .parent = window_parent,
    .decoration_hint = window_u32,
    .pointer_move_requested = window_move_requested,
    .pointer_resize_requested = window_resize_requested,
    .show_window_menu_requested = window_xy,
    .maximize_requested = window_none,
    .unmaximize_requested = window_none,
    .fullscreen_requested = window_fullscreen_requested,
    .exit_fullscreen_requested = window_none,
    .minimize_requested = window_none,
    .unreliable_pid = window_i32,
    .presentation_hint = window_u32,
    .identifier = window_str,
    .capture_sessions = window_u32,
};

/* --------------------------------------------------------------- manager */

static void
manager_window(void *data, struct river_window_manager_v1 *proxy,
               struct river_window_v1 *proxy_window)
{
	struct window *window;

	(void)data;
	(void)proxy;

	if (num_windows == MAX_WINDOWS) {
		river_window_v1_destroy(proxy_window);
		return;
	}

	window = &windows[num_windows++];
	memset(window, 0, sizeof(*window));
	window->proxy = proxy_window;
	window->node = river_window_v1_get_node(proxy_window);
	river_window_v1_add_listener(proxy_window, &window_listener, window);
	say("new window (%u total)", num_windows);
}

/* Step 2: propose a column layout, then finish. */
static void
manager_manage_start(void *data, struct river_window_manager_v1 *proxy)
{
	unsigned live = 0, row = 0, i;
	int32_t height;

	(void)data;

	for (i = 0; i < num_windows; ++i) {
		if (!windows[i].closed) {
			++live;
		}
	}

	if (live > 0) {
		height = screen_height / (int32_t)live;
		for (i = 0; i < num_windows; ++i) {
			if (windows[i].closed) {
				continue;
			}
			river_window_v1_propose_dimensions(windows[i].proxy, screen_width,
			                                   height);
			++row;
		}
	}

	say("manage_start: proposed dimensions for %u window(s)", live);
	river_window_manager_v1_manage_finish(proxy);
}

/* Step 5: position the nodes, then finish, which presents everything. */
static void
manager_render_start(void *data, struct river_window_manager_v1 *proxy)
{
	unsigned live = 0, row = 0, i;
	int32_t height;

	(void)data;

	for (i = 0; i < num_windows; ++i) {
		if (!windows[i].closed) {
			++live;
		}
	}

	if (live > 0) {
		height = screen_height / (int32_t)live;
		for (i = 0; i < num_windows; ++i) {
			if (windows[i].closed || !windows[i].node) {
				continue;
			}
			river_node_v1_set_position(windows[i].node, 0,
			                           (int32_t)row * height);
			river_node_v1_place_top(windows[i].node);
			++row;
		}
	}

	say("render_start: positioned %u window(s)", live);
	river_window_manager_v1_render_finish(proxy);
}

static void
manager_unavailable(void *data, struct river_window_manager_v1 *proxy)
{
	(void)data;
	(void)proxy;
	say("FAIL unavailable: another window manager is already running");
	running = false;
}

static void
manager_finished(void *data, struct river_window_manager_v1 *proxy)
{
	(void)data;
	(void)proxy;
	say("finished");
	running = false;
}

static void
manager_none(void *data, struct river_window_manager_v1 *proxy)
{
	(void)data;
	(void)proxy;
}

static void
output_removed(void *data, struct river_output_v1 *proxy)
{
	(void)data;
	say("output removed");
	river_output_v1_destroy(proxy);
}

static void
output_wl_output(void *data, struct river_output_v1 *proxy, uint32_t name)
{
	(void)data;
	(void)proxy;
	say("output wl_output global %u", name);
}

static void
output_position(void *data, struct river_output_v1 *proxy, int32_t x,
                int32_t y)
{
	(void)data;
	(void)proxy;
	say("output position %d,%d", x, y);
}

static void
output_dimensions(void *data, struct river_output_v1 *proxy, int32_t width,
                  int32_t height)
{
	(void)data;
	(void)proxy;
	screen_width = width;
	screen_height = height;
	say("output dimensions %dx%d", width, height);
}

static void
output_capture_sessions(void *data, struct river_output_v1 *proxy,
                        uint32_t count)
{
	(void)data;
	(void)proxy;
	(void)count;
}

static const struct river_output_v1_listener output_listener = {
    .removed = output_removed,
    .wl_output = output_wl_output,
    .position = output_position,
    .dimensions = output_dimensions,
    .capture_sessions = output_capture_sessions,
};

static void
manager_output(void *data, struct river_window_manager_v1 *proxy,
               struct river_output_v1 *output)
{
	(void)data;
	(void)proxy;
	river_output_v1_add_listener(output, &output_listener, NULL);
	say("new output");
}

static void
manager_seat(void *data, struct river_window_manager_v1 *proxy,
             struct river_seat_v1 *seat)
{
	(void)data;
	(void)proxy;
	(void)seat;
}

static const struct river_window_manager_v1_listener manager_listener = {
    .unavailable = manager_unavailable,
    .finished = manager_finished,
    .manage_start = manager_manage_start,
    .render_start = manager_render_start,
    .session_locked = manager_none,
    .session_unlocked = manager_none,
    .window = manager_window,
    .output = manager_output,
    .seat = manager_seat,
};

/* -------------------------------------------------------------- registry */

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name,
                const char *interface, uint32_t version)
{
	(void)data;

	if (strcmp(interface, river_window_manager_v1_interface.name) != 0) {
		return;
	}
	manager = wl_registry_bind(registry, name,
	                           &river_window_manager_v1_interface,
	                           version < 5 ? version : 5);
	river_window_manager_v1_add_listener(manager, &manager_listener, NULL);
	say("bound river_window_manager_v1 v%u", version);
}

static void
registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

int
main(int argc, char *argv[])
{
	struct wl_display *display;
	struct wl_registry *registry;

	(void)argc;
	(void)argv;

	display = wl_display_connect(NULL);
	if (!display) {
		say("FAIL cannot connect to the compositor");
		return EXIT_FAILURE;
	}

	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);

	if (!manager) {
		say("FAIL river_window_manager_v1 not advertised");
		return EXIT_FAILURE;
	}

	while (running && wl_display_dispatch(display) != -1) {
	}

	wl_display_disconnect(display);
	return EXIT_SUCCESS;
}
