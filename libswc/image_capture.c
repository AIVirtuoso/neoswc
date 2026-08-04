/* swc: libswc/image_capture.c
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
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 *
 *
 * ext-image-copy-capture-v1 and ext-image-capture-source-v1: screenshots and
 * screen recording. Before this there was no standard way to get a picture out
 * of the compositor at all -- swc ships its own swc_snap, which nothing but swc
 * speaks, so grim, wf-recorder and xdg-desktop-portal-wlr all had nothing to
 * talk to.
 *
 * Supported: whole-output capture into shm. Not supported: dmabuf (swc's
 * renderer path is software), cursor sessions, and foreign-toplevel sources --
 * each is advertised as absent rather than half-answered, so a client falls
 * back cleanly.
 */

#include "image_capture.h"
#include "compositor.h"
#include "internal.h"
#include "output.h"
#include "screen.h"
#include "shm.h"
#include "util.h"
#include "wayland_buffer.h"

#include "ext-image-capture-source-v1-server-protocol.h"
#include "ext-image-copy-capture-v1-server-protocol.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-server.h>
#include <wld/wld.h>

#define IMAGE_CAPTURE_VERSION 1

struct capture_source {
	struct wl_resource *resource;
	struct screen *screen;
	struct wl_listener screen_destroy_listener;
};

struct capture_session {
	struct wl_resource *resource;
	struct screen *screen;
	struct wl_resource *frame; /* at most one at a time */
	struct wl_listener screen_destroy_listener;
};

struct capture_frame {
	struct wl_resource *resource;
	struct capture_session *session;
	struct wld_buffer *buffer;
	bool captured;
};

/* ---------------------------------------------------------------- source */

static void
source_destroy_request(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct ext_image_capture_source_v1_interface source_impl = {
    .destroy = source_destroy_request,
};

static void
source_handle_screen_destroy(struct wl_listener *listener, void *data)
{
	struct capture_source *source =
	    wl_container_of(listener, source, screen_destroy_listener);

	(void)data;
	source->screen = NULL;
	wl_list_remove(&source->screen_destroy_listener.link);
	wl_list_init(&source->screen_destroy_listener.link);
}

static void
source_resource_destroy(struct wl_resource *resource)
{
	struct capture_source *source = wl_resource_get_user_data(resource);

	if (!source) {
		return;
	}
	wl_list_remove(&source->screen_destroy_listener.link);
	free(source);
}

static void
output_source_manager_create_source(struct wl_client *client,
                                    struct wl_resource *resource, uint32_t id,
                                    struct wl_resource *output_resource)
{
	struct capture_source *source;
	struct output *output = wl_resource_get_user_data(output_resource);

	if (!(source = calloc(1, sizeof(*source)))) {
		wl_client_post_no_memory(client);
		return;
	}
	source->screen = output ? output->screen : NULL;

	source->resource =
	    wl_resource_create(client, &ext_image_capture_source_v1_interface,
	                       wl_resource_get_version(resource), id);
	if (!source->resource) {
		free(source);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(source->resource, &source_impl, source,
	                               source_resource_destroy);

	source->screen_destroy_listener.notify = source_handle_screen_destroy;
	if (source->screen) {
		wl_signal_add(&source->screen->destroy_signal,
		              &source->screen_destroy_listener);
	} else {
		wl_list_init(&source->screen_destroy_listener.link);
	}
}

static void
output_source_manager_destroy(struct wl_client *client,
                              struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct ext_output_image_capture_source_manager_v1_interface
    output_source_manager_impl = {
        .create_source = output_source_manager_create_source,
        .destroy = output_source_manager_destroy,
};

static void
bind_output_source_manager(struct wl_client *client, void *data,
                           uint32_t version, uint32_t id)
{
	struct wl_resource *resource;

	(void)data;
	resource = wl_resource_create(
	    client, &ext_output_image_capture_source_manager_v1_interface, version,
	    id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &output_source_manager_impl, NULL,
	                               NULL);
}

/* ----------------------------------------------------------------- frame */

static void
frame_destroy_request(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
frame_attach_buffer(struct wl_client *client, struct wl_resource *resource,
                    struct wl_resource *buffer_resource)
{
	struct capture_frame *frame = wl_resource_get_user_data(resource);

	(void)client;
	if (!frame) {
		return;
	}
	if (frame->captured) {
		wl_resource_post_error(
		    resource, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_ALREADY_CAPTURED,
		    "attach_buffer after capture");
		return;
	}
	frame->buffer = buffer_resource ? wayland_buffer_get(buffer_resource) : NULL;
}

static void
frame_damage_buffer(struct wl_client *client, struct wl_resource *resource,
                    int32_t x, int32_t y, int32_t width, int32_t height)
{
	(void)client;
	(void)resource;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
	/*
	 * The client telling us which parts of its buffer are stale. We always copy
	 * the whole screen, so this changes nothing -- accepted so a client that
	 * tracks damage is not penalised for it.
	 */
}

static void
frame_capture(struct wl_client *client, struct wl_resource *resource)
{
	struct capture_frame *frame = wl_resource_get_user_data(resource);
	struct timespec now;

	(void)client;
	if (!frame) {
		return;
	}
	if (frame->captured) {
		wl_resource_post_error(
		    resource, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_ALREADY_CAPTURED,
		    "capture sent twice");
		return;
	}
	if (!frame->buffer) {
		wl_resource_post_error(
		    resource, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_NO_BUFFER,
		    "capture without a buffer");
		return;
	}
	frame->captured = true;

	if (!frame->session || !frame->session->screen ||
	    !compositor_copy_screen(frame->session->screen, frame->buffer)) {
		ext_image_copy_capture_frame_v1_send_failed(
		    resource, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
		return;
	}

	/*
	 * Whole-screen damage: we copy everything, so claiming less would be a lie
	 * a recorder could act on.
	 */
	ext_image_copy_capture_frame_v1_send_damage(
	    resource, 0, 0, (int32_t)frame->buffer->width,
	    (int32_t)frame->buffer->height);
	ext_image_copy_capture_frame_v1_send_transform(
	    resource, WL_OUTPUT_TRANSFORM_NORMAL);

	clock_gettime(CLOCK_MONOTONIC, &now);
	ext_image_copy_capture_frame_v1_send_presentation_time(
	    resource, (uint32_t)((uint64_t)now.tv_sec >> 32),
	    (uint32_t)now.tv_sec, (uint32_t)now.tv_nsec);

	ext_image_copy_capture_frame_v1_send_ready(resource);
}

static const struct ext_image_copy_capture_frame_v1_interface frame_impl = {
    .destroy = frame_destroy_request,
    .attach_buffer = frame_attach_buffer,
    .damage_buffer = frame_damage_buffer,
    .capture = frame_capture,
};

static void
frame_resource_destroy(struct wl_resource *resource)
{
	struct capture_frame *frame = wl_resource_get_user_data(resource);

	if (!frame) {
		return;
	}
	if (frame->session && frame->session->frame == resource) {
		frame->session->frame = NULL;
	}
	free(frame);
}

/* --------------------------------------------------------------- session */

static void
session_destroy_request(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
session_create_frame(struct wl_client *client, struct wl_resource *resource,
                     uint32_t id)
{
	struct capture_session *session = wl_resource_get_user_data(resource);
	struct capture_frame *frame;

	if (!session) {
		return;
	}
	if (session->frame) {
		wl_resource_post_error(
		    resource, EXT_IMAGE_COPY_CAPTURE_SESSION_V1_ERROR_DUPLICATE_FRAME,
		    "a frame already exists for this session");
		return;
	}
	if (!(frame = calloc(1, sizeof(*frame)))) {
		wl_client_post_no_memory(client);
		return;
	}
	frame->session = session;

	frame->resource =
	    wl_resource_create(client, &ext_image_copy_capture_frame_v1_interface,
	                       wl_resource_get_version(resource), id);
	if (!frame->resource) {
		free(frame);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(frame->resource, &frame_impl, frame,
	                               frame_resource_destroy);
	session->frame = frame->resource;
}

static const struct ext_image_copy_capture_session_v1_interface session_impl = {
    .create_frame = session_create_frame,
    .destroy = session_destroy_request,
};

static void
session_handle_screen_destroy(struct wl_listener *listener, void *data)
{
	struct capture_session *session =
	    wl_container_of(listener, session, screen_destroy_listener);

	(void)data;
	session->screen = NULL;
	wl_list_remove(&session->screen_destroy_listener.link);
	wl_list_init(&session->screen_destroy_listener.link);
	/* The source is gone for good; the client should tear the session down. */
	ext_image_copy_capture_session_v1_send_stopped(session->resource);
}

static void
session_resource_destroy(struct wl_resource *resource)
{
	struct capture_session *session = wl_resource_get_user_data(resource);

	if (!session) {
		return;
	}
	wl_list_remove(&session->screen_destroy_listener.link);
	free(session);
}

static void
session_send_config(struct capture_session *session)
{
	const struct swc_rectangle *geom;

	if (!session->screen) {
		ext_image_copy_capture_session_v1_send_stopped(session->resource);
		return;
	}
	geom = &session->screen->base.geometry;

	ext_image_copy_capture_session_v1_send_buffer_size(
	    session->resource, geom->width, geom->height);
	/*
	 * shm only. swc composites in software into a mapped buffer; there is no
	 * dmabuf export path, so advertising one would strand a client that
	 * preferred it. Both formats, since the scanout has no alpha but clients
	 * commonly ask for ARGB.
	 */
	ext_image_copy_capture_session_v1_send_shm_format(session->resource,
	                                                  WL_SHM_FORMAT_XRGB8888);
	ext_image_copy_capture_session_v1_send_shm_format(session->resource,
	                                                  WL_SHM_FORMAT_ARGB8888);
	ext_image_copy_capture_session_v1_send_done(session->resource);
}

/* --------------------------------------------------------------- manager */

static void
manager_create_session(struct wl_client *client, struct wl_resource *resource,
                       uint32_t id, struct wl_resource *source_resource,
                       uint32_t options)
{
	struct capture_source *source =
	    wl_resource_get_user_data(source_resource);
	struct capture_session *session;

	if (options & ~EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS) {
		wl_resource_post_error(
		    resource, EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_ERROR_INVALID_OPTION,
		    "unknown option bits");
		return;
	}
	/*
	 * paint_cursors is accepted and ignored: swc draws the cursor on a separate
	 * DRM plane, so it is not in the shadow buffer this reads and compositing it
	 * would mean a second copy path. A client asking for cursors gets frames
	 * without one, which is the same thing it gets from the flag being absent.
	 */

	if (!(session = calloc(1, sizeof(*session)))) {
		wl_client_post_no_memory(client);
		return;
	}
	session->screen = source ? source->screen : NULL;

	session->resource =
	    wl_resource_create(client, &ext_image_copy_capture_session_v1_interface,
	                       wl_resource_get_version(resource), id);
	if (!session->resource) {
		free(session);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(session->resource, &session_impl, session,
	                               session_resource_destroy);

	session->screen_destroy_listener.notify = session_handle_screen_destroy;
	if (session->screen) {
		wl_signal_add(&session->screen->destroy_signal,
		              &session->screen_destroy_listener);
	} else {
		wl_list_init(&session->screen_destroy_listener.link);
	}

	session_send_config(session);
}

static void
manager_create_pointer_cursor_session(struct wl_client *client,
                                      struct wl_resource *resource,
                                      uint32_t id,
                                      struct wl_resource *source,
                                      struct wl_resource *pointer)
{
	struct wl_resource *cursor;

	(void)resource;
	(void)source;
	(void)pointer;
	/*
	 * Created inert. swc's cursor lives on its own DRM plane and is never
	 * composited into a buffer, so there is nothing to copy. The object exists
	 * so a client is not disconnected for asking; it will simply never see an
	 * enter event.
	 */
	cursor = wl_resource_create(
	    client, &ext_image_copy_capture_cursor_session_v1_interface,
	    wl_resource_get_version(resource), id);
	if (!cursor) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(cursor, NULL, NULL, NULL);
}

static void
manager_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct ext_image_copy_capture_manager_v1_interface manager_impl = {
    .create_session = manager_create_session,
    .create_pointer_cursor_session = manager_create_pointer_cursor_session,
    .destroy = manager_destroy,
};

static void
bind_manager(struct wl_client *client, void *data, uint32_t version,
             uint32_t id)
{
	struct wl_resource *resource;

	(void)data;
	resource = wl_resource_create(
	    client, &ext_image_copy_capture_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

bool
image_capture_create(struct wl_display *display, struct wl_global **manager,
                     struct wl_global **output_source)
{
	*manager = wl_global_create(display,
	                            &ext_image_copy_capture_manager_v1_interface,
	                            IMAGE_CAPTURE_VERSION, NULL, &bind_manager);
	if (!*manager) {
		return false;
	}

	*output_source = wl_global_create(
	    display, &ext_output_image_capture_source_manager_v1_interface,
	    IMAGE_CAPTURE_VERSION, NULL, &bind_output_source_manager);
	if (!*output_source) {
		wl_global_destroy(*manager);
		*manager = NULL;
		return false;
	}

	return true;
}
