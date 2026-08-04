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
#include "river-xkb-bindings-v1-client-protocol.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include <sys/mman.h>
#include <unistd.h>

#define MAX_WINDOWS 16

struct window {
	struct river_window_v1 *proxy;
	struct river_node_v1 *node;
	int32_t width, height;
	bool closed;
};

static struct river_window_manager_v1 *manager;
static struct river_seat_v1 *the_seat;
static struct river_xkb_bindings_v1 *xkb_bindings;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct window windows[MAX_WINDOWS];
static unsigned num_windows;
static bool running = true;
static bool clip_test;
static bool csd_test;
static bool content_clip_test;
static bool clip_test_done;

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
	if (csd_test) {
		/* Client-side decorations mean the client draws its own title bar and
		 * borders, which clients do with subsurfaces -- the thing a clip test
		 * needs present in order to check whether children are clipped. */
		river_window_v1_use_csd(proxy_window);
	}
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
			if (!clip_test_done && (clip_test || content_clip_test)) {
				if (content_clip_test) {
					/* A visible border, so the screendump shows it
					 * reshaped around the clipped content. */
					river_window_v1_set_borders(windows[i].proxy, 0xf, 8,
					                             0xffff, 0, 0, 0xffff);
					river_window_v1_set_content_clip_box(
					    windows[i].proxy, 0, 0, 1, 1);
				} else {
					river_window_v1_set_clip_box(windows[i].proxy, 0, 0, 1,
					                             1);
				}
				clip_test_done = true;
				say("clip test: first window restricted to 1x1");
			}
			++row;
		}
	}

	/* Focus the most recently added window, which exercises the seat. */
	if (the_seat && live > 0) {
		for (i = num_windows; i > 0; --i) {
			if (!windows[i - 1].closed) {
				river_seat_v1_focus_window(the_seat, windows[i - 1].proxy);
				break;
			}
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
seat_removed(void *data, struct river_seat_v1 *proxy)
{
	(void)data;
	say("seat removed");
	river_seat_v1_destroy(proxy);
}

static void
seat_wl_seat(void *data, struct river_seat_v1 *proxy, uint32_t name)
{
	(void)data;
	(void)proxy;
	say("seat wl_seat global %u", name);
}

static void
seat_pointer_enter(void *data, struct river_seat_v1 *proxy,
                   struct river_window_v1 *window)
{
	(void)data;
	(void)proxy;
	(void)window;
	say("POINTER enter");
}

static void
seat_pointer_leave(void *data, struct river_seat_v1 *proxy)
{
	(void)data;
	(void)proxy;
	say("POINTER leave");
}

static void
seat_none(void *data, struct river_seat_v1 *proxy)
{
	(void)data;
	(void)proxy;
}

static void
seat_window(void *data, struct river_seat_v1 *proxy,
            struct river_window_v1 *window)
{
	(void)data;
	(void)proxy;
	(void)window;
}

static void
seat_shell_surface(void *data, struct river_seat_v1 *proxy,
                   struct river_shell_surface_v1 *shell_surface)
{
	(void)data;
	(void)proxy;
	(void)shell_surface;
}

static void
seat_xy(void *data, struct river_seat_v1 *proxy, int32_t x, int32_t y)
{
	(void)data;
	(void)proxy;
	(void)x;
	(void)y;
}

static const struct river_seat_v1_listener seat_listener = {
    .removed = seat_removed,
    .wl_seat = seat_wl_seat,
    .pointer_enter = seat_pointer_enter,
    .pointer_leave = seat_pointer_leave,
    .window_interaction = seat_window,
    .shell_surface_interaction = seat_shell_surface,
    .op_delta = seat_xy,
    .op_release = seat_none,
    .pointer_position = seat_xy,
};

/* ----------------------------------------------------------- key binding */

static void
binding_pressed(void *data, struct river_xkb_binding_v1 *proxy)
{
	(void)data;
	(void)proxy;
	say("BINDING pressed");
}

static void
binding_released(void *data, struct river_xkb_binding_v1 *proxy)
{
	(void)data;
	(void)proxy;
	say("BINDING released");
}

static void
binding_stop_repeat(void *data, struct river_xkb_binding_v1 *proxy)
{
	(void)data;
	(void)proxy;
}

static void
pointer_binding_pressed(void *data, struct river_pointer_binding_v1 *proxy)
{
	(void)data;
	(void)proxy;
	say("PBINDING pressed");
}

static void
pointer_binding_released(void *data, struct river_pointer_binding_v1 *proxy)
{
	(void)data;
	(void)proxy;
	say("PBINDING released");
}

static const struct river_pointer_binding_v1_listener pointer_binding_listener = {
    .pressed = pointer_binding_pressed,
    .released = pointer_binding_released,
};

static const struct river_xkb_binding_v1_listener binding_listener = {
    .pressed = binding_pressed,
    .released = binding_released,
    .stop_repeat = binding_stop_repeat,
};

/* Registered once a seat exists, and exercised with QEMU's `sendkey f1`. */
static void
register_test_binding(void)
{
	struct river_xkb_binding_v1 *binding;

	if (!xkb_bindings || !the_seat) {
		return;
	}

	/* XKB_KEY_F1, no modifiers. */
	binding = river_xkb_bindings_v1_get_xkb_binding(xkb_bindings, the_seat,
	                                               0xffbe, 0);
	river_xkb_binding_v1_add_listener(binding, &binding_listener, NULL);
	say("registered F1 binding");

	/* BTN_LEFT, no modifiers. */
	if (the_seat) {
		struct river_pointer_binding_v1 *pb =
		    river_seat_v1_get_pointer_binding(the_seat, 0x110, 0);
		river_pointer_binding_v1_add_listener(pb, &pointer_binding_listener,
		                                      NULL);
		say("registered BTN_LEFT binding");
	}
}

static void
manager_seat(void *data, struct river_window_manager_v1 *proxy,
             struct river_seat_v1 *seat)
{
	(void)data;
	(void)proxy;
	the_seat = seat;
	river_seat_v1_add_listener(seat, &seat_listener, NULL);
	say("new seat");
	register_test_binding();
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

/* --------------------------------------------------------- shell surface */

/*
 * A window manager's own surface: the compositor positions it, nothing
 * negotiates a size. Filled with a solid colour so it is unmistakable in a
 * screendump.
 */
static void
create_shell_surface(void)
{
	struct wl_surface *surface;
	struct river_shell_surface_v1 *shell;
	struct river_node_v1 *node;
	struct wl_shm_pool *pool;
	struct wl_buffer *buffer;
	uint32_t *pixels;
	int fd;
	size_t i, size;
	const int32_t w = 300, h = 120;

	if (!compositor || !shm || !manager) {
		return;
	}

	surface = wl_compositor_create_surface(compositor);
	shell = river_window_manager_v1_get_shell_surface(manager, surface);
	node = river_shell_surface_v1_get_node(shell);
	river_node_v1_set_position(node, 60, 60);

	size = (size_t)w * (size_t)h * 4;
	fd = memfd_create("wmclient-shell", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
		say("FAIL shell surface buffer");
		return;
	}
	pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) {
		say("FAIL shell surface mmap");
		return;
	}
	for (i = 0; i < size / 4; ++i) {
		pixels[i] = 0xffcc2222; /* opaque red */
	}

	pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	buffer = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4,
	                                   WL_SHM_FORMAT_ARGB8888);
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage(surface, 0, 0, w, h);
	wl_surface_commit(surface);

	say("SHELL created %dx%d at 60,60", w, h);
}

/* -------------------------------------------------------------- registry */

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name,
                const char *interface, uint32_t version)
{
	(void)data;

	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
		                             version < 4 ? version : 4);
		return;
	}

	if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
		return;
	}

	if (strcmp(interface, river_xkb_bindings_v1_interface.name) == 0) {
		xkb_bindings = wl_registry_bind(registry, name,
		                                &river_xkb_bindings_v1_interface,
		                                version < 3 ? version : 3);
		say("bound river_xkb_bindings_v1 v%u", version);
		register_test_binding();
		return;
	}

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
	const char *clip_mode;

	(void)argc;
	(void)argv;
	clip_mode = getenv("NEOSWC_TEST_CLIP");
	clip_test = clip_mode && strcmp(clip_mode, "full") == 0;
	content_clip_test = clip_mode && strcmp(clip_mode, "content") == 0;

	/*
	 * Ask clients to decorate themselves. Without this swc's server-side
	 * default wins whatever the client prefers, and a client like foot then
	 * creates no subsurfaces at all -- so a clip test has nothing but the
	 * toplevel to clip and cannot show whether children are clipped too.
	 */
	csd_test = getenv("NEOSWC_TEST_CSD") != NULL;

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

	wl_display_roundtrip(display);
	create_shell_surface();

	while (running && wl_display_dispatch(display) != -1) {
	}

	wl_display_disconnect(display);
	return EXIT_SUCCESS;
}
