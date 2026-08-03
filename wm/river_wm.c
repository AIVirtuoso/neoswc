/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 neoswc contributors
 *
 * river-window-management-v1, served on top of libswc's public API.
 *
 * The protocol's loop is:
 *
 *   1. server sends new state, then manage_start
 *   2. client changes window management state, then manage_finish
 *   3. server sends the new state to the windows and waits for them
 *   4. server sends the resulting dimensions, then render_start
 *   5. client changes rendering state, then render_finish
 *   6. server applies and displays
 *
 * Step 3 is swc's transaction barrier and step 6 is
 * swc_transaction_present(), which is why those were built first.
 */

#include "river_wm.h"

#include "river-window-management-v1-server-protocol.h"

#include <stdlib.h>
#include <string.h>
#include <swc.h>
#include <wayland-server.h>

/* How long step 3 waits for windows before giving up on the stragglers. */
static const uint32_t manage_timeout_ms = 100;

enum phase {
	PHASE_IDLE,
	PHASE_MANAGE,  /* manage_start sent, awaiting manage_finish */
	PHASE_SETTLE,  /* windows configured, awaiting the barrier */
	PHASE_RENDER,  /* render_start sent, awaiting render_finish */
};

struct river_window {
	struct wl_list link;
	struct wl_resource *resource; /* NULL until advertised */
	struct wl_resource *node;
	struct swc_window *swc;

	bool advertised;
	bool closed;

	/* Set by propose_dimensions during a manage sequence. */
	bool has_proposal;
	int32_t proposed_width, proposed_height;

	/* Last dimensions reported to the manager, to avoid repeats. */
	int32_t sent_width, sent_height;
};

struct river_output {
	struct wl_list link;
	struct wl_resource *resource; /* NULL until advertised */
	struct swc_screen *swc;
	bool advertised;
};

static struct {
	struct wl_display *display;
	struct wl_global *global;
	struct wl_resource *manager; /* the single bound client, or NULL */
	struct wl_event_source *idle;

	enum phase phase;
	bool dirty; /* state changed, or manage_dirty was requested */

	struct wl_list windows;
	struct wl_list outputs;
	struct swc_screen *screen;
} wm;

static void schedule_sequence(void);

/* ------------------------------------------------------------------ node */

static void
node_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
node_set_position(struct wl_client *client, struct wl_resource *resource,
                  int32_t x, int32_t y)
{
	struct river_window *window = wl_resource_get_user_data(resource);

	(void)client;
	if (!window || window->closed) {
		return;
	}
	swc_window_set_position(window->swc, x, y);
}

static void
node_place_top(struct wl_client *client, struct wl_resource *resource)
{
	struct river_window *window = wl_resource_get_user_data(resource);

	(void)client;
	if (window && !window->closed) {
		swc_window_raise(window->swc);
	}
}

static void
node_place_bottom(struct wl_client *client, struct wl_resource *resource)
{
	struct river_window *window = wl_resource_get_user_data(resource);

	(void)client;
	if (window && !window->closed) {
		swc_window_lower(window->swc);
	}
}

static void
node_place_relative(struct wl_resource *resource, struct wl_resource *sibling,
                    bool above)
{
	struct river_window *window = wl_resource_get_user_data(resource);
	struct river_window *other =
	    sibling ? wl_resource_get_user_data(sibling) : NULL;

	if (!window || !other || window->closed || other->closed) {
		return;
	}
	swc_window_restack(window->swc, other->swc, above);
}

static void
node_place_above(struct wl_client *client, struct wl_resource *resource,
                 struct wl_resource *sibling)
{
	(void)client;
	node_place_relative(resource, sibling, true);
}

static void
node_place_below(struct wl_client *client, struct wl_resource *resource,
                 struct wl_resource *sibling)
{
	(void)client;
	node_place_relative(resource, sibling, false);
}

static const struct river_node_v1_interface node_impl = {
    .destroy = node_destroy,
    .set_position = node_set_position,
    .place_top = node_place_top,
    .place_bottom = node_place_bottom,
    .place_above = node_place_above,
    .place_below = node_place_below,
};

static void
node_resource_destroy(struct wl_resource *resource)
{
	struct river_window *window = wl_resource_get_user_data(resource);

	if (window && window->node == resource) {
		window->node = NULL;
	}
}

/* ---------------------------------------------------------------- output */

static void
output_destroy_request(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
output_set_presentation_mode(struct wl_client *client,
                             struct wl_resource *resource, uint32_t mode)
{
	(void)client;
	(void)resource;
	(void)mode; /* tearing-control is not implemented */
}

static const struct river_output_v1_interface output_impl = {
    .destroy = output_destroy_request,
    .set_presentation_mode = output_set_presentation_mode,
};

static void
output_resource_destroy(struct wl_resource *resource)
{
	struct river_output *output = wl_resource_get_user_data(resource);

	if (output && output->resource == resource) {
		output->resource = NULL;
		output->advertised = false;
	}
}

/* Position and size, sent on advertisement and whenever they change. */
static void
send_output_geometry(struct river_output *output)
{
	const struct swc_rectangle *geometry;

	if (!output->resource) {
		return;
	}

	geometry = &output->swc->geometry;
	river_output_v1_send_position(output->resource, geometry->x, geometry->y);
	river_output_v1_send_dimensions(output->resource, (int32_t)geometry->width,
	                                (int32_t)geometry->height);
}

static void
advertise_output(struct river_output *output)
{
	struct wl_client *client;
	uint32_t name;

	if (output->advertised || !wm.manager) {
		return;
	}

	client = wl_resource_get_client(wm.manager);
	output->resource =
	    wl_resource_create(client, &river_output_v1_interface,
	                       wl_resource_get_version(wm.manager), 0);
	if (!output->resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(output->resource, &output_impl, output,
	                               output_resource_destroy);
	river_window_manager_v1_send_output(wm.manager, output->resource);
	output->advertised = true;

	/*
	 * Tell the manager which wl_output this is, so it can correlate with
	 * anything it learns from the registry. Only possible once the client has
	 * actually seen that global.
	 */
	if (swc_screen_get_wl_output_name(output->swc, client, &name)) {
		river_output_v1_send_wl_output(output->resource, name);
	}

	send_output_geometry(output);
}

/* ---------------------------------------------------------------- window */

static void
window_destroy_request(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
window_close(struct wl_client *client, struct wl_resource *resource)
{
	struct river_window *window = wl_resource_get_user_data(resource);

	(void)client;
	if (window && !window->closed) {
		swc_window_close(window->swc);
	}
}

static void
window_get_node(struct wl_client *client, struct wl_resource *resource,
                uint32_t id)
{
	struct river_window *window = wl_resource_get_user_data(resource);
	struct wl_resource *node;

	node = wl_resource_create(client, &river_node_v1_interface,
	                          wl_resource_get_version(resource), id);
	if (!node) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(node, &node_impl, window,
	                               node_resource_destroy);
	if (window) {
		window->node = node;
	}
}

static void
window_propose_dimensions(struct wl_client *client,
                          struct wl_resource *resource, int32_t width,
                          int32_t height)
{
	struct river_window *window = wl_resource_get_user_data(resource);

	(void)client;
	if (!window || window->closed) {
		return;
	}

	/*
	 * Recorded, not applied. The protocol requires every window in the
	 * sequence to be configured together at manage_finish, which is what
	 * makes the relayout atomic.
	 */
	window->has_proposal = true;
	window->proposed_width = width;
	window->proposed_height = height;
}

static void
window_hide(struct wl_client *client, struct wl_resource *resource)
{
	struct river_window *window = wl_resource_get_user_data(resource);

	(void)client;
	if (window && !window->closed) {
		swc_window_hide(window->swc);
	}
}

static void
window_show(struct wl_client *client, struct wl_resource *resource)
{
	struct river_window *window = wl_resource_get_user_data(resource);

	(void)client;
	if (window && !window->closed) {
		swc_window_show(window->swc);
	}
}

static void
window_fullscreen(struct wl_client *client, struct wl_resource *resource,
                  struct wl_resource *output)
{
	struct river_window *window = wl_resource_get_user_data(resource);

	(void)client;
	(void)output;
	if (window && !window->closed) {
		swc_window_set_fullscreen(window->swc, wm.screen);
	}
}

static void
window_exit_fullscreen(struct wl_client *client, struct wl_resource *resource)
{
	struct river_window *window = wl_resource_get_user_data(resource);

	(void)client;
	if (window && !window->closed) {
		swc_window_set_fullscreen(window->swc, NULL);
	}
}

/*
 * Requests accepted but not yet acted on. They must be present: a NULL entry
 * in the vtable makes libwayland dispatch through a null pointer.
 */
static void
window_ignore(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	(void)resource;
}

static void
window_use_csd(struct wl_client *client, struct wl_resource *resource)
{
	struct river_window *window = wl_resource_get_user_data(resource);

	(void)client;
	if (window && !window->closed) {
		swc_window_set_decoration_mode(window->swc,
		                               SWC_DECORATION_MODE_CLIENT_SIDE);
	}
}

static void
window_use_ssd(struct wl_client *client, struct wl_resource *resource)
{
	struct river_window *window = wl_resource_get_user_data(resource);

	(void)client;
	if (window && !window->closed) {
		swc_window_set_decoration_mode(window->swc,
		                               SWC_DECORATION_MODE_SERVER_SIDE);
	}
}

static void
window_set_borders(struct wl_client *client, struct wl_resource *resource,
                   uint32_t edges, int32_t width, uint32_t r, uint32_t g,
                   uint32_t b, uint32_t a)
{
	struct river_window *window = wl_resource_get_user_data(resource);
	uint32_t argb;

	(void)client;
	(void)edges; /* swc_window_set_border has one width for all edges */
	if (!window || window->closed) {
		return;
	}

	/* The protocol carries 16-bit premultiplied channels; swc wants 8-bit
	 * ARGB. */
	argb = ((a >> 8) & 0xff) << 24 | ((r >> 8) & 0xff) << 16 |
	       ((g >> 8) & 0xff) << 8 | ((b >> 8) & 0xff);
	swc_window_set_border(window->swc, argb, (uint32_t)(width < 0 ? 0 : width),
	                      0, 0);
}

static void
window_set_tiled(struct wl_client *client, struct wl_resource *resource,
                 uint32_t edges)
{
	struct river_window *window = wl_resource_get_user_data(resource);

	(void)client;
	(void)edges; /* swc has no per-edge tiled state yet */
	if (window && !window->closed) {
		swc_window_set_tiled(window->swc);
	}
}

static void
window_get_decoration(struct wl_client *client, struct wl_resource *resource,
                      uint32_t id, struct wl_resource *surface)
{
	struct wl_resource *decoration;

	(void)resource;
	(void)surface;
	/* Decoration surfaces are not implemented; hand back an inert object
	 * rather than killing the client. */
	decoration = wl_resource_create(client, &river_decoration_v1_interface,
	                                wl_resource_get_version(resource), id);
	if (!decoration) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(decoration, NULL, NULL, NULL);
}

static void
window_get_decoration_above(struct wl_client *client,
                            struct wl_resource *resource, uint32_t id,
                            struct wl_resource *surface)
{
	window_get_decoration(client, resource, id, surface);
}

static void
window_get_decoration_below(struct wl_client *client,
                            struct wl_resource *resource, uint32_t id,
                            struct wl_resource *surface)
{
	window_get_decoration(client, resource, id, surface);
}

static void
window_set_capabilities(struct wl_client *client, struct wl_resource *resource,
                        uint32_t caps)
{
	(void)client;
	(void)resource;
	(void)caps;
}

static void
window_set_box(struct wl_client *client, struct wl_resource *resource,
               int32_t x, int32_t y, int32_t width, int32_t height)
{
	(void)client;
	(void)resource;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
}

static void
window_set_dimension_bounds(struct wl_client *client,
                            struct wl_resource *resource, int32_t max_width,
                            int32_t max_height)
{
	(void)client;
	(void)resource;
	(void)max_width;
	(void)max_height;
}

static const struct river_window_v1_interface window_impl = {
    .destroy = window_destroy_request,
    .close = window_close,
    .get_node = window_get_node,
    .propose_dimensions = window_propose_dimensions,
    .hide = window_hide,
    .show = window_show,
    .use_csd = window_use_csd,
    .use_ssd = window_use_ssd,
    .set_borders = window_set_borders,
    .set_tiled = window_set_tiled,
    .get_decoration_above = window_get_decoration_above,
    .get_decoration_below = window_get_decoration_below,
    .inform_resize_start = window_ignore,
    .inform_resize_end = window_ignore,
    .set_capabilities = window_set_capabilities,
    .inform_maximized = window_ignore,
    .inform_unmaximized = window_ignore,
    .inform_fullscreen = window_ignore,
    .inform_not_fullscreen = window_ignore,
    .fullscreen = window_fullscreen,
    .exit_fullscreen = window_exit_fullscreen,
    .set_clip_box = window_set_box,
    .set_content_clip_box = window_set_box,
    .set_dimension_bounds = window_set_dimension_bounds,
};

static void
window_resource_destroy(struct wl_resource *resource)
{
	struct river_window *window = wl_resource_get_user_data(resource);

	if (window && window->resource == resource) {
		window->resource = NULL;
		window->advertised = false;
	}
}

/* ------------------------------------------------------------- sequencing */

static void
advertise_window(struct river_window *window)
{
	struct wl_client *client;
	struct swc_rectangle geometry;

	if (window->advertised || !wm.manager) {
		return;
	}

	client = wl_resource_get_client(wm.manager);
	window->resource =
	    wl_resource_create(client, &river_window_v1_interface,
	                       wl_resource_get_version(wm.manager), 0);
	if (!window->resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(window->resource, &window_impl, window,
	                               window_resource_destroy);
	river_window_manager_v1_send_window(wm.manager, window->resource);
	window->advertised = true;

	if (window->swc->title) {
		river_window_v1_send_title(window->resource, window->swc->title);
	}
	if (window->swc->app_id) {
		river_window_v1_send_app_id(window->resource, window->swc->app_id);
	}
	river_window_v1_send_dimensions_hint(
	    window->resource, (int32_t)window->swc->min_width,
	    (int32_t)window->swc->min_height, (int32_t)window->swc->max_width,
	    (int32_t)window->swc->max_height);

	if (swc_window_get_geometry(window->swc, &geometry)) {
		window->sent_width = (int32_t)geometry.width;
		window->sent_height = (int32_t)geometry.height;
		river_window_v1_send_dimensions(window->resource, window->sent_width,
		                                window->sent_height);
	}
}

static void
begin_manage(void)
{
	struct river_window *window;
	struct river_output *output;

	if (!wm.manager || wm.phase != PHASE_IDLE) {
		return;
	}

	/* Outputs first: a manager needs somewhere to put the windows. */
	wl_list_for_each (output, &wm.outputs, link) {
		advertise_output(output);
	}

	wl_list_for_each (window, &wm.windows, link) {
		advertise_window(window);
		window->has_proposal = false;
	}

	wm.dirty = false;
	wm.phase = PHASE_MANAGE;
	river_window_manager_v1_send_manage_start(wm.manager);
}

/* Report dimensions that changed, then hand over for the render sequence. */
static void
begin_render(bool timed_out, void *data)
{
	struct river_window *window;
	struct swc_rectangle geometry;

	(void)timed_out;
	(void)data;

	if (!wm.manager) {
		wm.phase = PHASE_IDLE;
		return;
	}

	wl_list_for_each (window, &wm.windows, link) {
		uint32_t width, height;

		if (!window->resource || window->closed) {
			continue;
		}

		/*
		 * The committed size, not the displayed one. At this point the window
		 * has acknowledged and committed its new size, but the render hold
		 * keeps that off the screen until render_finish -- so the view is
		 * still showing the previous size, and reporting it would put the
		 * manager a whole sequence behind.
		 */
		if (!swc_window_get_committed_size(window->swc, &width, &height)) {
			if (!swc_window_get_geometry(window->swc, &geometry)) {
				continue;
			}
			width = geometry.width;
			height = geometry.height;
		}

		if ((int32_t)width == window->sent_width &&
		    (int32_t)height == window->sent_height) {
			continue;
		}
		window->sent_width = (int32_t)width;
		window->sent_height = (int32_t)height;
		river_window_v1_send_dimensions(window->resource, window->sent_width,
		                                window->sent_height);
	}

	wm.phase = PHASE_RENDER;
	river_window_manager_v1_send_render_start(wm.manager);
}

static void
handle_idle(void *data)
{
	(void)data;
	wm.idle = NULL;
	begin_manage();
}

static void
schedule_sequence(void)
{
	wm.dirty = true;

	if (!wm.manager || wm.phase != PHASE_IDLE || wm.idle) {
		return;
	}

	/*
	 * Idle rather than immediate: several windows may appear in one dispatch,
	 * and they should share a sequence rather than each starting one.
	 */
	wm.idle = wl_event_loop_add_idle(wl_display_get_event_loop(wm.display),
	                                 handle_idle, NULL);
}

/* --------------------------------------------------------------- manager */

static void
manager_manage_finish(struct wl_client *client, struct wl_resource *resource)
{
	struct river_window *window;
	struct swc_rectangle geometry;

	(void)client;

	if (wm.phase != PHASE_MANAGE) {
		wl_resource_post_error(resource,
		                       RIVER_WINDOW_MANAGER_V1_ERROR_SEQUENCE_ORDER,
		                       "manage_finish outside a manage sequence");
		return;
	}

	wm.phase = PHASE_SETTLE;

	/* Step 3: configure every window as one cohort, then wait. */
	swc_transaction_begin();
	wl_list_for_each (window, &wm.windows, link) {
		if (!window->has_proposal || window->closed) {
			continue;
		}
		if (window->proposed_width <= 0 || window->proposed_height <= 0) {
			/* Zero means the window chooses; nothing to configure. */
			continue;
		}
		if (!swc_window_get_geometry(window->swc, &geometry)) {
			continue;
		}
		geometry.width = (uint32_t)window->proposed_width;
		geometry.height = (uint32_t)window->proposed_height;
		swc_window_set_geometry(window->swc, &geometry);
	}
	swc_transaction_commit(manage_timeout_ms, begin_render, NULL);
}

static void
manager_render_finish(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;

	if (wm.phase != PHASE_RENDER) {
		wl_resource_post_error(resource,
		                       RIVER_WINDOW_MANAGER_V1_ERROR_SEQUENCE_ORDER,
		                       "render_finish outside a render sequence");
		return;
	}

	/* Step 6: everything the cohort has been holding reaches the screen. */
	swc_transaction_present();
	wm.phase = PHASE_IDLE;

	if (wm.dirty) {
		schedule_sequence();
	}
}

static void
manager_manage_dirty(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	(void)resource;
	schedule_sequence();
}

static void
manager_stop(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	river_window_manager_v1_send_finished(resource);
}

static void
manager_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
manager_get_shell_surface(struct wl_client *client,
                          struct wl_resource *resource, uint32_t id,
                          struct wl_resource *surface)
{
	struct wl_resource *shell_surface;

	(void)surface;
	shell_surface =
	    wl_resource_create(client, &river_shell_surface_v1_interface,
	                       wl_resource_get_version(resource), id);
	if (!shell_surface) {
		wl_client_post_no_memory(client);
		return;
	}
	/* Shell surfaces are not implemented; an inert object beats a
	 * disconnected client. */
	wl_resource_set_implementation(shell_surface, NULL, NULL, NULL);
}

static void
manager_exit_session(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	(void)resource;
	wl_display_terminate(wm.display);
}

static const struct river_window_manager_v1_interface manager_impl = {
    .stop = manager_stop,
    .destroy = manager_destroy,
    .manage_finish = manager_manage_finish,
    .manage_dirty = manager_manage_dirty,
    .render_finish = manager_render_finish,
    .get_shell_surface = manager_get_shell_surface,
    .exit_session = manager_exit_session,
};

static void
manager_resource_destroy(struct wl_resource *resource)
{
	struct river_window *window;
	struct river_output *output;

	if (wm.manager != resource) {
		return;
	}

	wm.manager = NULL;
	wm.phase = PHASE_IDLE;

	/* Those objects belonged to that client; they went with it. A new manager
	 * will be told about everything again. */
	wl_list_for_each (window, &wm.windows, link) {
		window->resource = NULL;
		window->node = NULL;
		window->advertised = false;
	}
	wl_list_for_each (output, &wm.outputs, link) {
		output->resource = NULL;
		output->advertised = false;
	}
}

static void
bind_manager(struct wl_client *client, void *data, uint32_t version,
             uint32_t id)
{
	struct wl_resource *resource;

	(void)data;

	resource = wl_resource_create(client, &river_window_manager_v1_interface,
	                              (int)version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}

	if (wm.manager) {
		/*
		 * Only one window manager at a time. The protocol says unavailable is
		 * the first and only event on the object, so the implementation is
		 * left NULL: any request on it is a client bug.
		 */
		wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
		river_window_manager_v1_send_unavailable(resource);
		return;
	}

	wl_resource_set_implementation(resource, &manager_impl, NULL,
	                               manager_resource_destroy);
	wm.manager = resource;
	schedule_sequence();
}

/* ------------------------------------------------------------------ entry */

bool
river_wm_create(struct wl_display *display)
{
	wm.display = display;
	wm.phase = PHASE_IDLE;
	wl_list_init(&wm.windows);
	wl_list_init(&wm.outputs);

	wm.global = wl_global_create(display, &river_window_manager_v1_interface,
	                             5, NULL, bind_manager);
	return wm.global != NULL;
}

static void
handle_screen_destroy(void *data)
{
	struct river_output *output = data;

	if (output->resource) {
		river_output_v1_send_removed(output->resource);
	}
	if (wm.screen == output->swc) {
		wm.screen = NULL;
	}
	wl_list_remove(&output->link);
	free(output);
	schedule_sequence();
}

static void
handle_screen_geometry(void *data)
{
	struct river_output *output = data;

	send_output_geometry(output);
	schedule_sequence();
}

static const struct swc_screen_handler screen_handler = {
    .destroy = handle_screen_destroy,
    .geometry_changed = handle_screen_geometry,
    .usable_geometry_changed = handle_screen_geometry,
};

void
river_wm_add_screen(struct swc_screen *screen)
{
	struct river_output *output;

	output = calloc(1, sizeof(*output));
	if (!output) {
		return;
	}

	output->swc = screen;
	wl_list_insert(wm.outputs.prev, &output->link);
	swc_screen_set_handler(screen, &screen_handler, output);

	/* Windows are placed on this one until multi-output placement exists. */
	if (!wm.screen) {
		wm.screen = screen;
	}

	schedule_sequence();
}

static void
handle_window_destroy(void *data)
{
	struct river_window *window = data;

	window->closed = true;
	if (window->resource) {
		river_window_v1_send_closed(window->resource);
	}
	wl_list_remove(&window->link);
	free(window);
	schedule_sequence();
}

static void
handle_window_title(void *data)
{
	struct river_window *window = data;

	if (window->resource && window->swc->title) {
		river_window_v1_send_title(window->resource, window->swc->title);
		schedule_sequence();
	}
}

static void
handle_window_app_id(void *data)
{
	struct river_window *window = data;

	if (window->resource && window->swc->app_id) {
		river_window_v1_send_app_id(window->resource, window->swc->app_id);
		schedule_sequence();
	}
}

static const struct swc_window_handler window_handler = {
    .destroy = handle_window_destroy,
    .title_changed = handle_window_title,
    .app_id_changed = handle_window_app_id,
};

void
river_wm_add_window(struct swc_window *swc)
{
	struct river_window *window;

	window = calloc(1, sizeof(*window));
	if (!window) {
		return;
	}

	window->swc = swc;
	wl_list_insert(wm.windows.prev, &window->link);
	swc_window_set_handler(swc, &window_handler, window);
	swc_window_set_tiled(swc);
	swc_window_show(swc);

	schedule_sequence();
}
