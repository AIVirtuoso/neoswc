/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 neoswc contributors
 *
 * neoswc: a Wayland compositor serving river-window-management-v1.
 *
 * The compositor itself does no window management. It hands screens and
 * windows to river_wm.c, which exposes them to a single window manager client
 * over the protocol.
 */

#include "river_wm.h"

#include <stdio.h>
#include <stdlib.h>
#include <swc.h>
#include <wayland-server.h>

static void
new_screen(struct swc_screen *screen)
{
	river_wm_add_screen(screen);
}

static void
new_window(struct swc_window *window)
{
	river_wm_add_window(window);
}

static const struct swc_manager manager = {
    .new_screen = new_screen,
    .new_window = new_window,
};

int
main(int argc, char *argv[])
{
	struct wl_display *display;
	const char *socket;

	(void)argc;
	(void)argv;

	display = wl_display_create();
	if (!display) {
		fprintf(stderr, "neoswc: failed to create display\n");
		return EXIT_FAILURE;
	}

	socket = wl_display_add_socket_auto(display);
	if (!socket) {
		fprintf(stderr, "neoswc: failed to create socket\n");
		return EXIT_FAILURE;
	}
	setenv("WAYLAND_DISPLAY", socket, 1);
	fprintf(stderr, "neoswc: listening on %s\n", socket);

	/*
	 * Before swc_initialize: it calls new_screen for each screen it finds
	 * while still inside the call, so the manager's state has to exist by
	 * then.
	 */
	if (!river_wm_create(display)) {
		fprintf(stderr, "neoswc: failed to create the window manager global\n");
		return EXIT_FAILURE;
	}
	fprintf(stderr, "neoswc: river_window_manager_v1 advertised\n");

	if (!swc_initialize(display, NULL, &manager)) {
		fprintf(stderr, "neoswc: failed to initialize swc\n");
		return EXIT_FAILURE;
	}

	wl_display_run(display);
	wl_display_destroy(display);
	return EXIT_SUCCESS;
}
