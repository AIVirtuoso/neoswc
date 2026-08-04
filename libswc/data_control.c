/* swc: libswc/data_control.c
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
 * ext-data-control-v1: the clipboard for programs that are not the focused
 * window.
 *
 * wl_data_device only ever hands the selection to whichever client holds
 * keyboard focus. That is right for an application pasting into itself and
 * useless for everything else -- a clipboard manager, `wl-paste`, or any
 * library that wraps them has no window and never gets focus, so it can neither
 * read the selection nor be told when it changes.
 *
 * The practical effect of not having this: arboard (the Rust clipboard crate)
 * binds *only* data-control, fails to initialise, falls back to X11, and finds
 * no bridge there either, so clipboard support in such a program is simply
 * absent. wl-paste says so outright -- "The compositor does not seem to
 * implement %s, which is required for wl-clipboard to work".
 *
 * There is exactly one selection, shared with wl_data_device: a copy in a
 * terminal is readable by `wl-paste`, and a `wl-copy` is pastable into a
 * terminal. That sharing lives in data.c, which backs both source interfaces
 * with the same struct.
 *
 * Primary selection is deliberately absent rather than half-answered. swc has
 * no zwp_primary_selection_v1 either, so there is nothing to bridge to; the
 * protocol allows this exactly ("The compositor will ignore this request if it
 * does not support primary selection", and the primary_selection event is sent
 * only "if the compositor supports primary selection"), so set_primary_selection
 * is accepted and ignored and the event is never sent.
 */

#include "data_control.h"
#include "data.h"
#include "data_device.h"
#include "internal.h"
#include "seat.h"
#include "util.h"

#include "ext-data-control-v1-server-protocol.h"
#include <stdlib.h>
#include <wayland-server.h>

struct data_control_device {
	struct wl_resource *resource;
	struct wl_listener selection_listener;
	/* The offer currently advertised to this client, so it is not recreated
	 * when an unrelated event fires. Owned by the client once sent. */
	struct wl_resource *offer;
};

static void
device_send_selection(struct data_control_device *device)
{
	struct wl_client *client = wl_resource_get_client(device->resource);
	uint32_t version = wl_resource_get_version(device->resource);
	struct wl_resource *source = swc.seat->data_device->selection;
	struct wl_resource *offer = NULL;

	if (source) {
		offer = data_control_offer_new(client, source, version);
		if (!offer) {
			wl_client_post_no_memory(client);
			return;
		}
		ext_data_control_device_v1_send_data_offer(device->resource, offer);
		data_control_send_mime_types(source, offer);
	}

	device->offer = offer;
	ext_data_control_device_v1_send_selection(device->resource, offer);
}

static void
handle_selection_changed(struct wl_listener *listener, void *data)
{
	struct data_control_device *device =
	    wl_container_of(listener, device, selection_listener);
	struct event *ev = data;

	if (ev->type != DATA_DEVICE_EVENT_SELECTION_CHANGED) {
		return;
	}

	device_send_selection(device);
}

static void
device_set_selection(struct wl_client *client, struct wl_resource *resource,
                     struct wl_resource *source)
{
	(void)client;

	/*
	 * "The given source may not be used in any further set_selection or
	 * set_primary_selection requests." A source carries one selection; reusing
	 * it would leave two selections sharing one set of MIME types and one
	 * `send` handler, and the client could not tell which was being asked for.
	 */
	if (source && !data_source_mark_used(source)) {
		wl_resource_post_error(resource,
		                       EXT_DATA_CONTROL_DEVICE_V1_ERROR_USED_SOURCE,
		                       "data source already used");
		return;
	}

	data_device_set_selection(swc.seat->data_device, source);
}

static void
device_set_primary_selection(struct wl_client *client,
                             struct wl_resource *resource,
                             struct wl_resource *source)
{
	(void)client;
	(void)resource;
	(void)source;

	/* Accepted and ignored; see the note at the top of this file. Marking the
	 * source used anyway would be wrong -- it was never taken. */
}

static void
device_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct ext_data_control_device_v1_interface device_impl = {
    .set_selection = device_set_selection,
    .destroy = device_destroy,
    .set_primary_selection = device_set_primary_selection,
};

static void
device_resource_destroy(struct wl_resource *resource)
{
	struct data_control_device *device = wl_resource_get_user_data(resource);

	wl_list_remove(&device->selection_listener.link);
	free(device);
}

static void
manager_create_data_source(struct wl_client *client,
                           struct wl_resource *resource, uint32_t id)
{
	if (!data_control_source_new(client, wl_resource_get_version(resource),
	                             id)) {
		wl_client_post_no_memory(client);
	}
}

static void
manager_get_data_device(struct wl_client *client, struct wl_resource *resource,
                        uint32_t id, struct wl_resource *seat)
{
	struct data_control_device *device;

	(void)seat; /* swc is single-seat; there is one selection. */

	device = malloc(sizeof(*device));
	if (!device) {
		wl_client_post_no_memory(client);
		return;
	}

	device->resource =
	    wl_resource_create(client, &ext_data_control_device_v1_interface,
	                       wl_resource_get_version(resource), id);
	if (!device->resource) {
		free(device);
		wl_client_post_no_memory(client);
		return;
	}
	device->offer = NULL;
	wl_resource_set_implementation(device->resource, &device_impl, device,
	                               device_resource_destroy);

	device->selection_listener.notify = handle_selection_changed;
	wl_signal_add(&swc.seat->data_device->event_signal,
	              &device->selection_listener);

	/* "The first selection event is sent upon binding the
	 * ext_data_control_device object." A client that binds after a copy has
	 * already happened must still see it, so this is not optional. */
	device_send_selection(device);
}

static void
manager_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct ext_data_control_manager_v1_interface manager_impl = {
    .create_data_source = manager_create_data_source,
    .get_data_device = manager_get_data_device,
    .destroy = manager_destroy,
};

static void
bind_data_control(struct wl_client *client, void *data, uint32_t version,
                  uint32_t id)
{
	struct wl_resource *resource;

	(void)data;

	resource = wl_resource_create(
	    client, &ext_data_control_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

struct wl_global *
data_control_create(struct wl_display *display)
{
	return wl_global_create(display, &ext_data_control_manager_v1_interface, 1,
	                        NULL, &bind_data_control);
}
