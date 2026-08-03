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

#include "river-layer-shell-v1-server-protocol.h"
#include "river-window-management-v1-server-protocol.h"
#include "river-xkb-bindings-v1-server-protocol.h"

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
	struct wl_resource *layer_shell; /* river_layer_shell_output_v1 */
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
	struct wl_resource *seat;
	struct wl_resource *layer_seat;
	struct wl_global *layer_shell_global;
	struct wl_global *xkb_bindings_global;
	struct swc_screen *screen;
} wm;

static void schedule_sequence(void);
static void maybe_free_window(struct river_window *window);

/* ----------------------------------------------------------------- inert */

/*
 * Objects handed back for features that are not implemented.
 *
 * They need a real vtable of no-ops. Passing NULL to
 * wl_resource_set_implementation() does not make an object inert -- libwayland
 * disconnects the client on the first request to it, with
 * "Implementation of resource N of <interface> is NULL". The point of handing
 * these back at all is to avoid killing a manager that merely asked, so a NULL
 * implementation defeats the purpose exactly.
 */

static void
inert_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
inert_none(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	(void)resource;
}

static void
inert_new_id(struct wl_client *client, struct wl_resource *resource,
             uint32_t id)
{
	(void)client;
	(void)resource;
	(void)id;
}

static void
inert_xy(struct wl_client *client, struct wl_resource *resource, int32_t x,
         int32_t y)
{
	(void)client;
	(void)resource;
	(void)x;
	(void)y;
}

/* ---------------------------------------------------------- shell surface */

/*
 * The window manager's own surfaces -- bars, overlays, menus it draws itself.
 * Unlike a window there is no shell protocol negotiating a size: the manager
 * positions it and the client just draws.
 */
struct river_shell_surface {
	struct wl_resource *resource;
	struct wl_resource *node;
	struct swc_shell_surface *swc;
};

static void
shell_node_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
shell_node_set_position(struct wl_client *client, struct wl_resource *resource,
                        int32_t x, int32_t y)
{
	struct river_shell_surface *shell = wl_resource_get_user_data(resource);

	(void)client;
	if (shell && shell->swc) {
		swc_shell_surface_set_position(shell->swc, x, y);
	}
}

static void
shell_node_place_top(struct wl_client *client, struct wl_resource *resource)
{
	struct river_shell_surface *shell = wl_resource_get_user_data(resource);

	(void)client;
	if (shell && shell->swc) {
		swc_shell_surface_raise(shell->swc);
	}
}

static void
shell_node_place_bottom(struct wl_client *client, struct wl_resource *resource)
{
	struct river_shell_surface *shell = wl_resource_get_user_data(resource);

	(void)client;
	if (shell && shell->swc) {
		swc_shell_surface_lower(shell->swc);
	}
}

/*
 * Relative placement would need to order a shell surface against a window,
 * which are different objects on the swc side with no shared restack call.
 * Raise or lower within the layer instead, which is what a bar or overlay
 * actually needs.
 */
static void
shell_node_place_above(struct wl_client *client, struct wl_resource *resource,
                       struct wl_resource *sibling)
{
	(void)sibling;
	shell_node_place_top(client, resource);
}

static void
shell_node_place_below(struct wl_client *client, struct wl_resource *resource,
                       struct wl_resource *sibling)
{
	(void)sibling;
	shell_node_place_bottom(client, resource);
}

static const struct river_node_v1_interface shell_node_impl = {
    .destroy = shell_node_destroy,
    .set_position = shell_node_set_position,
    .place_top = shell_node_place_top,
    .place_bottom = shell_node_place_bottom,
    .place_above = shell_node_place_above,
    .place_below = shell_node_place_below,
};

static void
shell_node_resource_destroy(struct wl_resource *resource)
{
	struct river_shell_surface *shell = wl_resource_get_user_data(resource);

	if (shell && shell->node == resource) {
		shell->node = NULL;
	}
}

static void
shell_surface_destroy_request(struct wl_client *client,
                              struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
shell_surface_get_node(struct wl_client *client, struct wl_resource *resource,
                       uint32_t id)
{
	struct river_shell_surface *shell = wl_resource_get_user_data(resource);
	struct wl_resource *node;

	if (shell && shell->node) {
		wl_resource_post_error(
		    resource, RIVER_SHELL_SURFACE_V1_ERROR_NODE_EXISTS,
		    "shell surface already has a node");
		return;
	}

	node = wl_resource_create(client, &river_node_v1_interface,
	                          wl_resource_get_version(resource), id);
	if (!node) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(node, &shell_node_impl, shell,
	                               shell_node_resource_destroy);
	if (shell) {
		shell->node = node;
	}
}

static void
shell_surface_sync_next_commit(struct wl_client *client,
                               struct wl_resource *resource)
{
	(void)client;
	(void)resource;
	/*
	 * Holds the surface's next commit until the manager's render_finish, the
	 * same guarantee the render hold gives windows. Not wired up: the hold is
	 * driven from the window cohort, and a shell surface is not part of one.
	 */
}

static const struct river_shell_surface_v1_interface shell_surface_impl = {
    .destroy = shell_surface_destroy_request,
    .get_node = shell_surface_get_node,
    .sync_next_commit = shell_surface_sync_next_commit,
};

static void
shell_surface_resource_destroy(struct wl_resource *resource)
{
	struct river_shell_surface *shell = wl_resource_get_user_data(resource);

	if (!shell) {
		return;
	}
	swc_shell_surface_destroy(shell->swc);
	if (shell->node) {
		wl_resource_set_user_data(shell->node, NULL);
	}
	free(shell);
}

static const struct river_decoration_v1_interface decoration_impl = {
    .destroy = inert_destroy,
    .set_offset = inert_xy,
    .sync_next_commit = inert_none,
};

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

	if (!window || window->node != resource) {
		return;
	}

	window->node = NULL;
	maybe_free_window(window);
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

	if (output->resource) {
		geometry = &output->swc->geometry;
		river_output_v1_send_position(output->resource, geometry->x,
		                              geometry->y);
		river_output_v1_send_dimensions(
		    output->resource, (int32_t)geometry->width,
		    (int32_t)geometry->height);
	}

	/*
	 * swc's usable_geometry is the screen minus the exclusive zones of layer
	 * surfaces, which is exactly what non_exclusive_area means. It changes
	 * whenever a panel docks or undocks, so this is sent from the same place
	 * as the geometry.
	 */
	if (output->layer_shell) {
		geometry = &output->swc->usable_geometry;
		river_layer_shell_output_v1_send_non_exclusive_area(
		    output->layer_shell, geometry->x, geometry->y,
		    (int32_t)geometry->width, (int32_t)geometry->height);
	}
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

/* ---------------------------------------------------------- xkb bindings */

/*
 * Keyboard bindings on top of swc_add_binding().
 *
 * The manager registers a keysym plus modifiers and is told when it is pressed
 * and released; swc swallows the key so it never reaches the focused client.
 * That is exactly what swc's binding API already does, so this is mostly
 * translation.
 */

/*
 * Shared by key and pointer bindings: swc treats them the same, with a keysym
 * or a button code in the same slot, so only the events sent differ.
 */
struct river_binding {
	struct wl_resource *resource;
	enum swc_binding_type type;
	uint32_t keysym; /* or button code, for SWC_BINDING_BUTTON */
	uint32_t modifiers; /* swc's encoding, already translated */
	bool registered;
};

/*
 * river uses the X11 mask convention (shift 1, ctrl 4, mod1 8, mod4 64); swc
 * has its own (ctrl 1, alt 2, logo 4, shift 8). Neither is a subset of the
 * other, so translate rather than cast.
 */
static uint32_t
modifiers_to_swc(uint32_t modifiers)
{
	uint32_t result = 0;

	if (modifiers & RIVER_SEAT_V1_MODIFIERS_SHIFT) {
		result |= SWC_MOD_SHIFT;
	}
	if (modifiers & RIVER_SEAT_V1_MODIFIERS_CTRL) {
		result |= SWC_MOD_CTRL;
	}
	if (modifiers & RIVER_SEAT_V1_MODIFIERS_MOD1) {
		result |= SWC_MOD_ALT;
	}
	if (modifiers & RIVER_SEAT_V1_MODIFIERS_MOD4) {
		result |= SWC_MOD_LOGO;
	}
	/* mod3 and mod5 have no swc equivalent and are dropped. */

	return result;
}

static void
binding_pressed(void *data, uint32_t time, uint32_t value, uint32_t state)
{
	struct river_binding *binding = data;

	(void)time;
	(void)value;

	if (!binding->resource) {
		return;
	}

	if (binding->type == SWC_BINDING_BUTTON) {
		if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
			river_pointer_binding_v1_send_pressed(binding->resource);
		} else {
			river_pointer_binding_v1_send_released(binding->resource);
		}
		return;
	}

	if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		river_xkb_binding_v1_send_pressed(binding->resource);
	} else {
		river_xkb_binding_v1_send_released(binding->resource);
	}
}

static void
binding_register(struct river_binding *binding)
{
	if (binding->registered) {
		return;
	}
	if (swc_add_binding(binding->type, binding->modifiers, binding->keysym,
	                    binding_pressed, binding) == 0) {
		binding->registered = true;
	}
}

static void
binding_unregister(struct river_binding *binding)
{
	if (!binding->registered) {
		return;
	}
	swc_remove_binding(binding->type, binding->modifiers, binding->keysym);
	binding->registered = false;
}

static void
binding_destroy_request(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
binding_enable(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	binding_register(wl_resource_get_user_data(resource));
}

static void
binding_disable(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	binding_unregister(wl_resource_get_user_data(resource));
}

static void
binding_set_layout_override(struct wl_client *client,
                            struct wl_resource *resource, uint32_t layout)
{
	(void)client;
	(void)resource;
	(void)layout;
	/*
	 * Resolve the keysym in a specific keyboard layout rather than the active
	 * one, so a binding keeps working when the layout changes. swc's bindings
	 * match on the keysym the active layout produced, with no way to ask for
	 * another, so this is accepted and ignored.
	 */
}

static const struct river_pointer_binding_v1_interface pointer_binding_impl = {
    .destroy = binding_destroy_request,
    .enable = binding_enable,
    .disable = binding_disable,
};

static const struct river_xkb_binding_v1_interface binding_impl = {
    .destroy = binding_destroy_request,
    .set_layout_override = binding_set_layout_override,
    .enable = binding_enable,
    .disable = binding_disable,
};

static void
binding_resource_destroy(struct wl_resource *resource)
{
	struct river_binding *binding = wl_resource_get_user_data(resource);

	if (!binding) {
		return;
	}
	binding_unregister(binding);
	binding->resource = NULL;
	free(binding);
}

static void
xkb_seat_destroy_request(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
xkb_seat_ignore(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	(void)resource;
}

static void
xkb_seat_modifiers_watch(struct wl_client *client, struct wl_resource *resource,
                         uint32_t modifiers)
{
	(void)client;
	(void)resource;
	(void)modifiers;
	/*
	 * modifiers_update is never sent: swc reports modifier state to focused
	 * clients but exposes no hook for the compositor to observe it. A manager
	 * using this to drive modifier-held modes will not see them.
	 */
}

static const struct river_xkb_bindings_seat_v1_interface xkb_seat_impl = {
    .destroy = xkb_seat_destroy_request,
    /*
     * ensure_next_key_eaten asks that the next key press be swallowed rather
     * than delivered, which swc's binding API cannot express: it swallows keys
     * that match a registered binding and nothing else. ate_unbound_key is
     * therefore never sent either.
     */
    .ensure_next_key_eaten = xkb_seat_ignore,
    .cancel_ensure_next_key_eaten = xkb_seat_ignore,
    .modifiers_watch = xkb_seat_modifiers_watch,
};

static void
xkb_bindings_destroy_request(struct wl_client *client,
                             struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
xkb_bindings_get_xkb_binding(struct wl_client *client,
                             struct wl_resource *resource,
                             struct wl_resource *seat, uint32_t id,
                             uint32_t keysym, uint32_t modifiers)
{
	struct river_binding *binding;

	(void)seat; /* single seat */

	binding = calloc(1, sizeof(*binding));
	if (!binding) {
		wl_client_post_no_memory(client);
		return;
	}
	binding->type = SWC_BINDING_KEY;
	binding->keysym = keysym;
	binding->modifiers = modifiers_to_swc(modifiers);

	binding->resource =
	    wl_resource_create(client, &river_xkb_binding_v1_interface,
	                       wl_resource_get_version(resource), id);
	if (!binding->resource) {
		free(binding);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(binding->resource, &binding_impl, binding,
	                               binding_resource_destroy);

	/* The protocol says a binding starts enabled. */
	binding_register(binding);
}

static void
xkb_bindings_get_seat(struct wl_client *client, struct wl_resource *resource,
                      uint32_t id, struct wl_resource *seat)
{
	struct wl_resource *xkb_seat;

	(void)seat;

	xkb_seat = wl_resource_create(client,
	                              &river_xkb_bindings_seat_v1_interface,
	                              wl_resource_get_version(resource), id);
	if (!xkb_seat) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(xkb_seat, &xkb_seat_impl, NULL, NULL);
}

static const struct river_xkb_bindings_v1_interface xkb_bindings_impl = {
    .destroy = xkb_bindings_destroy_request,
    .get_xkb_binding = xkb_bindings_get_xkb_binding,
    .get_seat = xkb_bindings_get_seat,
};

static void
bind_xkb_bindings(struct wl_client *client, void *data, uint32_t version,
                  uint32_t id)
{
	struct wl_resource *resource;

	(void)data;

	resource = wl_resource_create(client, &river_xkb_bindings_v1_interface,
	                              (int)version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &xkb_bindings_impl, NULL, NULL);
}

/* ----------------------------------------------------------- layer shell */

/*
 * river's layer shell, which is not zwlr_layer_shell_v1. That one is a client
 * protocol swc already implements; this one tells the *window manager* about
 * layer surfaces so it can lay windows out around them.
 *
 * Binding the global is how a manager declares it handles layer surfaces at
 * all. swc places them itself via layer_shell.c and reflects the result in
 * each screen's usable_geometry, so what the manager needs from us is that
 * area -- which is exactly the non_exclusive_area event.
 */

static void
layer_output_destroy_request(struct wl_client *client,
                             struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
layer_output_set_default(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	(void)resource;
	/*
	 * Which output an unplaced layer surface lands on. swc decides that in
	 * layer_shell.c without consulting anyone, so recording the manager's
	 * preference would not change the outcome. Accepted so the manager is not
	 * disconnected for expressing it.
	 */
}

static const struct river_layer_shell_output_v1_interface layer_output_impl = {
    .destroy = layer_output_destroy_request,
    .set_default = layer_output_set_default,
};

static void
layer_output_resource_destroy(struct wl_resource *resource)
{
	struct river_output *output = wl_resource_get_user_data(resource);

	if (output && output->layer_shell == resource) {
		output->layer_shell = NULL;
	}
}

static void
layer_seat_destroy_request(struct wl_client *client,
                           struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct river_layer_shell_seat_v1_interface layer_seat_impl = {
    .destroy = layer_seat_destroy_request,
};

static void
layer_seat_resource_destroy(struct wl_resource *resource)
{
	if (wm.layer_seat == resource) {
		wm.layer_seat = NULL;
	}
}

static void
layer_shell_destroy_request(struct wl_client *client,
                            struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
layer_shell_get_output(struct wl_client *client, struct wl_resource *resource,
                       uint32_t id, struct wl_resource *output_resource)
{
	struct river_output *output = wl_resource_get_user_data(output_resource);
	struct wl_resource *layer_output;

	if (output && output->layer_shell) {
		wl_resource_post_error(
		    resource, RIVER_LAYER_SHELL_V1_ERROR_OBJECT_ALREADY_CREATED,
		    "layer shell output already created for this output");
		return;
	}

	layer_output =
	    wl_resource_create(client, &river_layer_shell_output_v1_interface,
	                       wl_resource_get_version(resource), id);
	if (!layer_output) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(layer_output, &layer_output_impl, output,
	                               layer_output_resource_destroy);

	if (output) {
		output->layer_shell = layer_output;
		send_output_geometry(output);
	}
}

static void
layer_shell_get_seat(struct wl_client *client, struct wl_resource *resource,
                     uint32_t id, struct wl_resource *seat_resource)
{
	struct wl_resource *layer_seat;

	(void)seat_resource;

	if (wm.layer_seat) {
		wl_resource_post_error(
		    resource, RIVER_LAYER_SHELL_V1_ERROR_OBJECT_ALREADY_CREATED,
		    "layer shell seat already created for this seat");
		return;
	}

	layer_seat = wl_resource_create(client,
	                                &river_layer_shell_seat_v1_interface,
	                                wl_resource_get_version(resource), id);
	if (!layer_seat) {
		wl_client_post_no_memory(client);
		return;
	}
	/*
	 * The focus_exclusive / focus_non_exclusive / focus_none events are never
	 * sent: swc's layer_shell.c decides keyboard interactivity for layer
	 * surfaces itself and reports nothing, so there is nothing to forward. A
	 * manager relying on those to track layer focus will not see it.
	 */
	wl_resource_set_implementation(layer_seat, &layer_seat_impl, NULL,
	                               layer_seat_resource_destroy);
	wm.layer_seat = layer_seat;
}

static const struct river_layer_shell_v1_interface layer_shell_impl = {
    .destroy = layer_shell_destroy_request,
    .get_output = layer_shell_get_output,
    .get_seat = layer_shell_get_seat,
};

static void
bind_layer_shell(struct wl_client *client, void *data, uint32_t version,
                 uint32_t id)
{
	struct wl_resource *resource;

	(void)data;

	resource = wl_resource_create(client, &river_layer_shell_v1_interface,
	                              (int)version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &layer_shell_impl, NULL, NULL);
}

/* ------------------------------------------------------------------ seat */

static void
seat_destroy_request(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
seat_focus_window(struct wl_client *client, struct wl_resource *resource,
                  struct wl_resource *window_resource)
{
	struct river_window *window;

	(void)client;
	(void)resource;

	if (!window_resource) {
		swc_window_focus(NULL);
		return;
	}

	window = wl_resource_get_user_data(window_resource);
	if (window && !window->closed) {
		swc_window_focus(window->swc);
	}
}

static void
seat_clear_focus(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	(void)resource;
	swc_window_focus(NULL);
}

static void
seat_ignore(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	(void)resource;
}

static void
seat_focus_shell_surface(struct wl_client *client,
                         struct wl_resource *resource,
                         struct wl_resource *shell_surface)
{
	(void)client;
	(void)resource;
	(void)shell_surface; /* shell surfaces are not implemented */
}

static void
seat_get_pointer_binding(struct wl_client *client, struct wl_resource *resource,
                         uint32_t id, uint32_t button, uint32_t modifiers)
{
	struct river_binding *binding;

	binding = calloc(1, sizeof(*binding));
	if (!binding) {
		wl_client_post_no_memory(client);
		return;
	}
	/* Same shape as a key binding, with a button code instead of a keysym. */
	binding->type = SWC_BINDING_BUTTON;
	binding->keysym = button;
	binding->modifiers = modifiers_to_swc(modifiers);

	binding->resource =
	    wl_resource_create(client, &river_pointer_binding_v1_interface,
	                       wl_resource_get_version(resource), id);
	if (!binding->resource) {
		free(binding);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(binding->resource, &pointer_binding_impl,
	                               binding, binding_resource_destroy);

	binding_register(binding);
}

static void
seat_set_xcursor_theme(struct wl_client *client, struct wl_resource *resource,
                       const char *theme, uint32_t size)
{
	(void)client;
	(void)resource;
	(void)theme;
	(void)size; /* xcursor theme loading is Tier 2 */
}

static void
seat_pointer_warp(struct wl_client *client, struct wl_resource *resource,
                  int32_t x, int32_t y)
{
	(void)client;
	(void)resource;
	(void)x;
	(void)y; /* no public pointer warp in swc yet */
}

static const struct river_seat_v1_interface seat_impl = {
    .destroy = seat_destroy_request,
    .focus_window = seat_focus_window,
    .focus_shell_surface = seat_focus_shell_surface,
    .clear_focus = seat_clear_focus,
    .op_start_pointer = seat_ignore,
    .op_end = seat_ignore,
    .get_pointer_binding = seat_get_pointer_binding,
    .set_xcursor_theme = seat_set_xcursor_theme,
    .pointer_warp = seat_pointer_warp,
};

static void
seat_resource_destroy(struct wl_resource *resource)
{
	if (wm.seat == resource) {
		wm.seat = NULL;
	}
}

static void
advertise_seat(void)
{
	struct wl_client *client;
	uint32_t name;

	if (wm.seat || !wm.manager) {
		return;
	}

	client = wl_resource_get_client(wm.manager);
	wm.seat = wl_resource_create(client, &river_seat_v1_interface,
	                             wl_resource_get_version(wm.manager), 0);
	if (!wm.seat) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(wm.seat, &seat_impl, NULL,
	                               seat_resource_destroy);
	river_window_manager_v1_send_seat(wm.manager, wm.seat);

	/* swc is single-seat, so there is exactly one of these. */
	if (swc_get_wl_seat_name(client, &name)) {
		river_seat_v1_send_wl_seat(wm.seat, name);
	}
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
		/*
		 * Only the newest node is tracked. Strand any earlier one by
		 * clearing its user data, which every node handler already treats as
		 * inert -- otherwise it keeps a pointer this window may outlive.
		 */
		if (window->node) {
			wl_resource_set_user_data(window->node, NULL);
		}
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
	wl_resource_set_implementation(decoration, &decoration_impl, NULL, NULL);
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

	if (!window || window->resource != resource) {
		return;
	}

	window->resource = NULL;
	window->advertised = false;
	maybe_free_window(window);
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

	advertise_seat();

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
	struct river_shell_surface *shell;

	shell = calloc(1, sizeof(*shell));
	if (!shell) {
		wl_client_post_no_memory(client);
		return;
	}

	/* NULL if the wl_surface already has a role, which the protocol treats as
	 * an error rather than something to tolerate. */
	shell->swc = swc_shell_surface_create(surface);
	if (!shell->swc) {
		free(shell);
		wl_resource_post_error(resource, RIVER_WINDOW_MANAGER_V1_ERROR_ROLE,
		                       "wl_surface already has a role");
		return;
	}

	shell->resource =
	    wl_resource_create(client, &river_shell_surface_v1_interface,
	                       wl_resource_get_version(resource), id);
	if (!shell->resource) {
		swc_shell_surface_destroy(shell->swc);
		free(shell);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(shell->resource, &shell_surface_impl, shell,
	                               shell_surface_resource_destroy);

	/* A manager's own interface belongs above ordinary windows. */
	swc_shell_surface_set_stack_layer(shell->swc, SWC_STACK_LAYER_TOP);
	swc_shell_surface_show(shell->swc);
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
	wm.seat = NULL;
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
	if (!wm.global) {
		return false;
	}

	/* Advertising this is how the compositor says layer surfaces are handled
	 * by the manager rather than dropped. */
	wm.layer_shell_global = wl_global_create(
	    display, &river_layer_shell_v1_interface, 1, NULL, bind_layer_shell);
	if (!wm.layer_shell_global) {
		return false;
	}

	wm.xkb_bindings_global =
	    wl_global_create(display, &river_xkb_bindings_v1_interface, 3, NULL,
	                     bind_xkb_bindings);
	return wm.xkb_bindings_global != NULL;
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

/*
 * A river_window outlives its swc window.
 *
 * The protocol has the client destroy the river_window_v1 after it receives
 * closed, so between those two points the resource is still live and its user
 * data must stay valid. Freeing on the swc side leaves the resource pointing
 * at freed memory, and any request on it -- or its eventual destruction --
 * takes the compositor down. Free only once both sides are gone.
 */
static void
maybe_free_window(struct river_window *window)
{
	/*
	 * The node counts too. It is a separate resource holding this same
	 * pointer as user data, and it can outlive both the swc window and the
	 * river_window_v1 -- a manager that keeps a node around and repositions
	 * it would then reach freed memory, which is a crash inside
	 * swc_window_set_position rather than anywhere obviously wrong here.
	 */
	if (window->swc || window->resource || window->node) {
		return;
	}
	free(window);
}

static void
handle_window_destroy(void *data)
{
	struct river_window *window = data;

	window->closed = true;
	window->swc = NULL;
	wl_list_remove(&window->link);
	wl_list_init(&window->link);

	if (window->resource) {
		river_window_v1_send_closed(window->resource);
	}

	maybe_free_window(window);
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

static void
handle_window_entered(void *data)
{
	struct river_window *window = data;

	/*
	 * swc reports the pointer entering a window but has no matching "left",
	 * so pointer_leave is never sent. A manager using pointer_enter for
	 * focus-follows-mouse still works; one that tracks enter/leave pairs will
	 * not. Needs a leave callback in swc (Tier 2).
	 */
	if (wm.seat && window->resource) {
		river_seat_v1_send_pointer_enter(wm.seat, window->resource);
	}
}

static void
handle_window_left(void *data)
{
	struct river_window *window = data;

	(void)window;
	if (wm.seat) {
		river_seat_v1_send_pointer_leave(wm.seat);
	}
}

static const struct swc_window_handler window_handler = {
    .destroy = handle_window_destroy,
    .title_changed = handle_window_title,
    .app_id_changed = handle_window_app_id,
    .entered = handle_window_entered,
    .left = handle_window_left,
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
