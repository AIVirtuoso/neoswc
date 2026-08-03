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

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <swc.h>
#include <sys/wait.h>
#include <unistd.h>
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

/*
 * The manager is an ordinary Wayland client, so it cannot be the argument to
 * swc-launch -- that slot belongs to the compositor. On a bare VT there is
 * also no shell and no terminal to start one from, so the compositor spawns it
 * itself: `swc-launch -- neoswc rill`.
 */
static char *const *manager_command;
static pid_t manager_pid = -1;

static int
handle_sigchld(int signal_number, void *data)
{
	struct wl_display *display = data;
	pid_t pid;
	int status;

	(void)signal_number;

	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		if (pid != manager_pid)
			continue;
		if (WIFEXITED(status)) {
			fprintf(stderr, "neoswc: manager '%s' exited with status %d\n",
			        manager_command[0], WEXITSTATUS(status));
		} else if (WIFSIGNALED(status)) {
			fprintf(stderr, "neoswc: manager '%s' killed by signal %d\n",
			        manager_command[0], WTERMSIG(status));
		}
		/*
		 * Without a manager nothing can be laid out and nothing can be
		 * started, so a compositor that kept running would just hold the
		 * VT behind a black screen. Quit and give the VT back.
		 */
		wl_display_terminate(display);
	}

	return 0;
}

static bool
spawn_manager(void)
{
	pid_t pid;

	pid = fork();
	if (pid == -1) {
		fprintf(stderr, "neoswc: fork: %s\n", strerror(errno));
		return false;
	}
	if (pid == 0) {
		sigset_t set;

		/* wl_event_loop_add_signal blocks SIGCHLD in this process, and a
		 * fork inherits the mask. The manager spawns processes of its
		 * own and would never see them exit. swc-launch hands the
		 * compositor an empty mask; hand the manager the same. */
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);

		execvp(manager_command[0], manager_command);
		fprintf(stderr, "neoswc: failed to exec '%s': %s\n",
		        manager_command[0], strerror(errno));
		_exit(EXIT_FAILURE);
	}

	manager_pid = pid;
	fprintf(stderr, "neoswc: spawned manager '%s' as pid %d\n",
	        manager_command[0], (int)pid);
	return true;
}

int
main(int argc, char *argv[])
{
	struct wl_display *display;
	const char *socket;

	if (argc > 1) {
		manager_command = argv + 1;
	}

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

	/* After swc_initialize, so every global the manager binds already
	 * exists by the time its registry request is answered. */
	if (manager_command) {
		/* wl_event_loop_add_signal blocks the signal and reads it from a
		 * signalfd, so the reaping happens in the event loop rather than
		 * in a handler interrupting it. */
		if (!wl_event_loop_add_signal(wl_display_get_event_loop(display),
		                              SIGCHLD, handle_sigchld, display)) {
			fprintf(stderr, "neoswc: failed to watch for the manager exiting\n");
			return EXIT_FAILURE;
		}
		if (!spawn_manager()) {
			return EXIT_FAILURE;
		}
	} else {
		fprintf(stderr,
		        "neoswc: no manager given, so nothing will be laid out or shown.\n"
		        "neoswc: pass one -- 'swc-launch -- neoswc rill' -- or connect a\n"
		        "neoswc: river window manager to %s yourself.\n",
		        socket);
	}

	wl_display_run(display);
	wl_display_destroy(display);
	return EXIT_SUCCESS;
}
