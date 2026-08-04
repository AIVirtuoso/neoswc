/* swc: data.c
 *
 * Copyright (c) 2013-2020 Michael Forney
 *
 * Modifications copyright (c) 2026 neoswc contributors
 *
 * SPDX-License-Identifier: MIT AND GPL-3.0-or-later
 *
 * The MIT notice below covers the original upstream code. Modifications by
 * neoswc contributors are licensed GPL-3.0-or-later; see COPYING.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "data.h"
#include "util.h"

#include "ext-data-control-v1-server-protocol.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server.h>

/*
 * One struct backs both kinds of selection source.
 *
 * ext_data_control_source_v1 is a different interface from wl_data_source, but
 * it holds exactly the same thing: a list of MIME types and a way to be asked
 * for the bytes. Giving them one representation is what lets a selection set
 * through either protocol be read through the other, which is the whole point
 * of data-control -- and it costs one enum, rather than an abstract source type
 * threaded through data_device.c, seat.c and every caller.
 */
enum source_kind {
	SOURCE_WL_DATA,      /* wl_data_source */
	SOURCE_DATA_CONTROL, /* ext_data_control_source_v1 */
};

struct data {
	struct wl_array mime_types;
	struct wl_resource *source;
	enum source_kind kind;
	/* A data-control source may be handed to set_selection only once. */
	bool used;
	struct wl_list offers;
};

static void
offer_accept(struct wl_client *client, struct wl_resource *offer,
             uint32_t serial, const char *mime_type)
{
	struct data *data = wl_resource_get_user_data(offer);

	/* Protect against expired data_offers being used. */
	if (!data) {
		return;
	}

	/* Drag-and-drop feedback; ext_data_control_source_v1 has no equivalent
	 * event, and a clipboard read has nothing to accept anyway. */
	if (data->kind == SOURCE_WL_DATA) {
		wl_data_source_send_target(data->source, mime_type);
	}
}

/*
 * Ask the source for the bytes, whichever protocol it came from.
 *
 * Takes ownership of fd. The source hands it to its own client, which writes
 * and closes it; this end closes its copy immediately, so the reader sees EOF
 * when the writer is done rather than hanging on a descriptor we forgot.
 */
static void
source_send(struct data *data, const char *mime_type, int fd)
{
	if (data->kind == SOURCE_DATA_CONTROL) {
		ext_data_control_source_v1_send_send(data->source, mime_type, fd);
	} else {
		wl_data_source_send_send(data->source, mime_type, fd);
	}
	close(fd);
}

static void
offer_receive(struct wl_client *client, struct wl_resource *offer,
              const char *mime_type, int fd)
{
	struct data *data = wl_resource_get_user_data(offer);

	/* Protect against expired data_offers being used. */
	if (!data) {
		close(fd);
		return;
	}

	source_send(data, mime_type, fd);
}

static void
offer_finish(struct wl_client *client, struct wl_resource *offer)
{
	(void)client;
	(void)offer;
	/* TODO: Implement */
}

static void
offer_set_actions(struct wl_client *client, struct wl_resource *offer, uint32_t dnd_actions, uint32_t preferred_action)
{
	(void)client;
	(void)offer;
	(void)dnd_actions;
	(void)preferred_action;
	/* TODO: Implement */
}

static const struct wl_data_offer_interface data_offer_impl = {
	.accept = offer_accept,
	.receive = offer_receive,
	.destroy = destroy_resource,
	.finish = offer_finish,
	.set_actions = offer_set_actions,
};

static void
source_offer(struct wl_client *client, struct wl_resource *source,
             const char *mime_type)
{
	struct data *data = wl_resource_get_user_data(source);
	char *s, **dst;

	s = strdup(mime_type);
	if (!s) {
		goto error0;
	}
	dst = wl_array_add(&data->mime_types, sizeof(*dst));
	if (!dst) {
		goto error1;
	}
	*dst = s;
	return;

error1:
	free(s);
error0:
	wl_resource_post_no_memory(source);
}

static void
source_set_actions(struct wl_client *client, struct wl_resource *resource, uint32_t dnd_actions)
{
	(void)client;
	(void)resource;
	(void)dnd_actions;
	/* TODO: Implement */
}

static const struct wl_data_source_interface data_source_impl = {
	.offer = source_offer,
	.destroy = destroy_resource,
	.set_actions = source_set_actions,
};

/* Same two requests, same handlers -- ext_data_control_source_v1 is
 * wl_data_source without the drag-and-drop half. */
static const struct ext_data_control_source_v1_interface
    data_control_source_impl = {
        .offer = source_offer,
        .destroy = destroy_resource,
};

static void
data_control_offer_receive(struct wl_client *client, struct wl_resource *offer,
                           const char *mime_type, int fd)
{
	struct data *data = wl_resource_get_user_data(offer);

	(void)client;

	if (!data) {
		close(fd);
		return;
	}

	source_send(data, mime_type, fd);
}

static const struct ext_data_control_offer_v1_interface
    data_control_offer_impl = {
        .receive = data_control_offer_receive,
        .destroy = destroy_resource,
};

static void
data_destroy(struct wl_resource *source)
{
	struct data *data = wl_resource_get_user_data(source);
	struct wl_resource *offer;
	char **mime_type;

	wl_array_for_each(mime_type, &data->mime_types) free(*mime_type);
	wl_array_release(&data->mime_types);

	/* After this data_source is destroyed, each of the data_offer objects
	 * associated with the data_source has a pointer to a free'd struct. We
	 * can't destroy the resources because this results in a segfault on the
	 * client when it correctly tries to call data_source.destroy. However, a
	 * misbehaving client could still attempt to call accept or receive on the
	 * data_offer, which would crash the server.
	 *
	 * So, we clear the user data on each of the offers to protect us. */
	wl_resource_for_each(offer, &data->offers)
	{
		wl_resource_set_user_data(offer, NULL);
		wl_resource_set_destructor(offer, NULL);
	}

	free(data);
}

static struct data *
data_new(void)
{
	struct data *data;

	data = malloc(sizeof(*data));
	if (!data) {
		return NULL;
	}
	wl_array_init(&data->mime_types);
	wl_list_init(&data->offers);
	data->kind = SOURCE_WL_DATA;
	data->used = false;

	return data;
}

struct wl_resource *
data_source_new(struct wl_client *client, uint32_t version, uint32_t id)
{
	struct data *data;

	data = data_new();
	if (!data) {
		goto error0;
	}

	data->source =
	    wl_resource_create(client, &wl_data_source_interface, version, id);
	if (!data->source) {
		goto error1;
	}
	wl_resource_set_implementation(data->source, &data_source_impl, data,
	                               &data_destroy);

	return data->source;

error1:
	free(data);
error0:
	return NULL;
}

struct wl_resource *
data_control_source_new(struct wl_client *client, uint32_t version, uint32_t id)
{
	struct data *data;

	data = data_new();
	if (!data) {
		goto error0;
	}
	data->kind = SOURCE_DATA_CONTROL;

	data->source = wl_resource_create(
	    client, &ext_data_control_source_v1_interface, version, id);
	if (!data->source) {
		goto error1;
	}
	wl_resource_set_implementation(data->source, &data_control_source_impl,
	                               data, &data_destroy);

	return data->source;

error1:
	free(data);
error0:
	return NULL;
}

struct wl_resource *
data_offer_new(struct wl_client *client, struct wl_resource *source,
               uint32_t version)
{
	struct data *data = wl_resource_get_user_data(source);
	struct wl_resource *offer;

	offer = wl_resource_create(client, &wl_data_offer_interface, version, 0);
	if (!offer) {
		return NULL;
	}
	wl_resource_set_implementation(offer, &data_offer_impl, data,
	                               &remove_resource);
	wl_list_insert(&data->offers, wl_resource_get_link(offer));

	return offer;
}

void
data_send_mime_types(struct wl_resource *source, struct wl_resource *offer)
{
	struct data *data = wl_resource_get_user_data(source);
	char **mime_type;

	wl_array_for_each(mime_type, &data->mime_types)
	    wl_data_offer_send_offer(offer, *mime_type);
}

struct wl_resource *
data_control_offer_new(struct wl_client *client, struct wl_resource *source,
                       uint32_t version)
{
	struct data *data = wl_resource_get_user_data(source);
	struct wl_resource *offer;

	offer = wl_resource_create(client, &ext_data_control_offer_v1_interface,
	                           version, 0);
	if (!offer) {
		return NULL;
	}
	wl_resource_set_implementation(offer, &data_control_offer_impl, data,
	                               &remove_resource);
	/* The same list data_destroy() walks to blank expired offers, which it
	 * does by resource id and so does not care which interface they are. */
	wl_list_insert(&data->offers, wl_resource_get_link(offer));

	return offer;
}

void
data_control_send_mime_types(struct wl_resource *source,
                             struct wl_resource *offer)
{
	struct data *data = wl_resource_get_user_data(source);
	char **mime_type;

	wl_array_for_each(mime_type, &data->mime_types)
	    ext_data_control_offer_v1_send_offer(offer, *mime_type);
}

/*
 * Tell a source it is no longer the selection.
 *
 * data_device.c used to call wl_data_source_send_cancelled() directly, which
 * would be a wrong-interface event once a data-control source can hold the
 * selection -- libwayland would send the wl_data_source opcode to an
 * ext_data_control_source_v1 object and the client would abort.
 */
void
data_source_cancel(struct wl_resource *source)
{
	struct data *data = wl_resource_get_user_data(source);

	if (!data) {
		return;
	}

	if (data->kind == SOURCE_DATA_CONTROL) {
		ext_data_control_source_v1_send_cancelled(source);
	} else {
		wl_data_source_send_cancelled(source);
	}
}

bool
data_source_mark_used(struct wl_resource *source)
{
	struct data *data = wl_resource_get_user_data(source);

	if (!data || data->used) {
		return false;
	}
	data->used = true;

	return true;
}
