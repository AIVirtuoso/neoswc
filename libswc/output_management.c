/* swc: libswc/output_management.c
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
 * zwlr_output_management_v1 -- how kanshi, wlr-randr and friends arrange
 * monitors. wayland-protocols has no equivalent, staging or otherwise, so this
 * wlroots protocol is the only one those tools speak.
 *
 * What is supported: reading the head layout, and moving heads.
 * swc positions screens left to right in connector enumeration order, which has
 * nothing to do with how the monitors sit on the desk, and set_position is what
 * fixes that.
 *
 * What is not: mode switching, scaling, transforms and disabling a head. Those
 * need a DRM modeset or scaling swc does not do, so a configuration asking for
 * any of them is rejected with `failed` rather than silently half-applied. A
 * configuration that names the head's *current* mode is accepted, since that is
 * what a config file pinning the existing mode produces.
 */

#include "output_management.h"
#include "internal.h"
#include "mode.h"
#include "output.h"
#include "screen.h"
#include "util.h"

#include "wlr-output-management-unstable-v1-server-protocol.h"
#include <stdlib.h>
#include <string.h>
#include <wayland-server.h>

/* The version we advertise. 2 adds make/model/serial_number; 3 adds release
 * requests; 4 adds adaptive sync, which swc has no notion of. */
#define OUTPUT_MANAGEMENT_VERSION 2

struct output_manager {
	struct wl_resource *resource;
	struct wl_list heads;
	struct wl_list link;
};

struct output_head {
	struct wl_resource *resource;
	struct screen *screen;
	struct wl_list modes;
	struct wl_list link;
};

struct output_head_mode {
	struct wl_resource *resource;
	struct mode *mode;
	struct wl_list link;
};

struct output_configuration {
	struct wl_resource *resource;
	uint32_t serial;
	struct wl_list heads;
	bool used;
};

struct output_config_head {
	struct wl_resource *resource;
	struct screen *screen;
	bool disabled;
	bool position_set;
	int32_t x, y;
	/* Anything we cannot honour. Recorded rather than answered immediately,
	 * because the protocol wants one succeeded/failed for the whole
	 * configuration, not a per-property answer. */
	bool unsupported;
	struct wl_list link;
};

static struct wl_list managers;
static uint32_t current_serial;

/* ------------------------------------------------------------------- heads */

static struct output *
screen_output(struct screen *screen)
{
	struct output *output;

	wl_list_for_each (output, &screen->outputs, link) {
		return output;
	}
	return NULL;
}

static void
head_release(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct zwlr_output_head_v1_interface head_impl = {
    .release = head_release,
};

static void
head_mode_release(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct zwlr_output_mode_v1_interface head_mode_impl = {
    .release = head_mode_release,
};

static void
head_mode_resource_destroy(struct wl_resource *resource)
{
	struct output_head_mode *mode = wl_resource_get_user_data(resource);

	if (!mode) {
		return;
	}
	wl_list_remove(&mode->link);
	free(mode);
}

static void
head_resource_destroy(struct wl_resource *resource)
{
	struct output_head *head = wl_resource_get_user_data(resource);
	struct output_head_mode *mode, *tmp;

	if (!head) {
		return;
	}
	wl_list_for_each_safe (mode, tmp, &head->modes, link) {
		wl_resource_set_user_data(mode->resource, NULL);
		wl_list_remove(&mode->link);
		free(mode);
	}
	wl_list_remove(&head->link);
	free(head);
}

/* Everything the client needs to describe one screen. */
static void
send_head(struct output_manager *manager, struct screen *screen)
{
	struct wl_client *client = wl_resource_get_client(manager->resource);
	uint32_t version = wl_resource_get_version(manager->resource);
	struct output *output = screen_output(screen);
	struct output_head *head;
	struct output_head_mode *hm;
	struct mode *mode;
	struct mode *current = &screen->planes.primary.mode;

	if (!output) {
		return;
	}
	if (!(head = calloc(1, sizeof(*head)))) {
		return;
	}
	head->screen = screen;
	wl_list_init(&head->modes);

	head->resource =
	    wl_resource_create(client, &zwlr_output_head_v1_interface, version, 0);
	if (!head->resource) {
		free(head);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(head->resource, &head_impl, head,
	                               head_resource_destroy);
	wl_list_insert(&manager->heads, &head->link);

	zwlr_output_manager_v1_send_head(manager->resource, head->resource);
	zwlr_output_head_v1_send_name(head->resource, output->name);
	zwlr_output_head_v1_send_description(head->resource, output->name);
	if (output->physical_width || output->physical_height) {
		zwlr_output_head_v1_send_physical_size(
		    head->resource, (int32_t)output->physical_width,
		    (int32_t)output->physical_height);
	}

	wl_array_for_each (mode, &output->modes) {
		if (!(hm = calloc(1, sizeof(*hm)))) {
			continue;
		}
		hm->mode = mode;
		hm->resource = wl_resource_create(
		    client, &zwlr_output_mode_v1_interface, version, 0);
		if (!hm->resource) {
			free(hm);
			continue;
		}
		wl_resource_set_implementation(hm->resource, &head_mode_impl, hm,
		                               head_mode_resource_destroy);
		wl_list_insert(&head->modes, &hm->link);

		zwlr_output_head_v1_send_mode(head->resource, hm->resource);
		zwlr_output_mode_v1_send_size(hm->resource, mode->width, mode->height);
		if (mode->refresh) {
			zwlr_output_mode_v1_send_refresh(hm->resource,
			                                 (int32_t)mode->refresh);
		}
		if (mode->preferred) {
			zwlr_output_mode_v1_send_preferred(hm->resource);
		}
		if (mode_equal(current, mode)) {
			zwlr_output_head_v1_send_current_mode(head->resource, hm->resource);
		}
	}

	/* swc cannot turn a screen off, so a head is always enabled. */
	zwlr_output_head_v1_send_enabled(head->resource, 1);
	zwlr_output_head_v1_send_position(head->resource, screen->base.geometry.x,
	                                  screen->base.geometry.y);
	zwlr_output_head_v1_send_transform(head->resource,
	                                   WL_OUTPUT_TRANSFORM_NORMAL);
	zwlr_output_head_v1_send_scale(head->resource, wl_fixed_from_int(1));

	if (version >= ZWLR_OUTPUT_HEAD_V1_MAKE_SINCE_VERSION) {
		/* swc does not read EDID, so there is no make or model to report.
		 * The name is stable and unique, which is what a config file matches
		 * on anyway. */
		zwlr_output_head_v1_send_make(head->resource, "unknown");
		zwlr_output_head_v1_send_model(head->resource, output->name);
		zwlr_output_head_v1_send_serial_number(head->resource, "unknown");
	}
}

static void
manager_send_all(struct output_manager *manager)
{
	struct screen *screen;

	wl_list_for_each (screen, &swc.screens, link) {
		send_head(manager, screen);
	}
	zwlr_output_manager_v1_send_done(manager->resource, current_serial);
}

/* --------------------------------------------------------- configurations */

static struct output_config_head *
config_head_get(struct output_configuration *config, struct screen *screen)
{
	struct output_config_head *ch;

	wl_list_for_each (ch, &config->heads, link) {
		if (ch->screen == screen) {
			return ch;
		}
	}
	return NULL;
}

static void
config_head_set_mode(struct wl_client *client, struct wl_resource *resource,
                     struct wl_resource *mode_resource)
{
	struct output_config_head *ch = wl_resource_get_user_data(resource);
	struct output_head_mode *hm = wl_resource_get_user_data(mode_resource);

	(void)client;
	if (!ch) {
		return;
	}
	/*
	 * Accepted when it names the mode already in use -- a config file that
	 * pins the current mode is the common case and must not fail. Refresh is
	 * not compared: kanshi's `mode 2560x1440` picks whichever mode object has
	 * those dimensions, which need not be the exact one in use.
	 */
	if (!hm || !hm->mode || hm->mode->width != ch->screen->planes.primary.mode.width ||
	    hm->mode->height != ch->screen->planes.primary.mode.height) {
		ch->unsupported = true;
	}
}

static void
config_head_set_custom_mode(struct wl_client *client,
                            struct wl_resource *resource, int32_t width,
                            int32_t height, int32_t refresh)
{
	struct output_config_head *ch = wl_resource_get_user_data(resource);

	(void)client;
	(void)width;
	(void)height;
	(void)refresh;
	if (ch) {
		ch->unsupported = true;
	}
}

static void
config_head_set_position(struct wl_client *client, struct wl_resource *resource,
                         int32_t x, int32_t y)
{
	struct output_config_head *ch = wl_resource_get_user_data(resource);

	(void)client;
	if (ch) {
		ch->position_set = true;
		ch->x = x;
		ch->y = y;
	}
}

static void
config_head_set_transform(struct wl_client *client,
                          struct wl_resource *resource, int32_t transform)
{
	struct output_config_head *ch = wl_resource_get_user_data(resource);

	(void)client;
	if (!ch) {
		return;
	}
	if (transform != WL_OUTPUT_TRANSFORM_NORMAL) {
		ch->unsupported = true;
	}
}

static void
config_head_set_scale(struct wl_client *client, struct wl_resource *resource,
                      wl_fixed_t scale)
{
	struct output_config_head *ch = wl_resource_get_user_data(resource);

	(void)client;
	if (!ch) {
		return;
	}
	if (scale != wl_fixed_from_int(1)) {
		ch->unsupported = true;
	}
}

static void
config_head_set_adaptive_sync(struct wl_client *client,
                              struct wl_resource *resource, uint32_t state)
{
	struct output_config_head *ch = wl_resource_get_user_data(resource);

	(void)client;
	if (ch && state != ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_DISABLED) {
		ch->unsupported = true;
	}
}

static const struct zwlr_output_configuration_head_v1_interface
    config_head_impl = {
        .set_mode = config_head_set_mode,
        .set_custom_mode = config_head_set_custom_mode,
        .set_position = config_head_set_position,
        .set_transform = config_head_set_transform,
        .set_scale = config_head_set_scale,
        .set_adaptive_sync = config_head_set_adaptive_sync,
};

static void
config_head_resource_destroy(struct wl_resource *resource)
{
	struct output_config_head *ch = wl_resource_get_user_data(resource);

	if (!ch) {
		return;
	}
	wl_list_remove(&ch->link);
	free(ch);
}

static struct output_config_head *
config_add_head(struct output_configuration *config, struct wl_client *client,
                struct wl_resource *config_resource,
                struct wl_resource *head_resource, uint32_t id, bool disabled)
{
	struct output_head *head = wl_resource_get_user_data(head_resource);
	struct output_config_head *ch;

	if (!head) {
		/* The screen went away between the head event and this request. */
		return NULL;
	}
	if (config_head_get(config, head->screen)) {
		wl_resource_post_error(
		    config_resource,
		    ZWLR_OUTPUT_CONFIGURATION_V1_ERROR_ALREADY_CONFIGURED_HEAD,
		    "head already configured");
		return NULL;
	}
	if (!(ch = calloc(1, sizeof(*ch)))) {
		wl_client_post_no_memory(client);
		return NULL;
	}
	ch->screen = head->screen;
	ch->disabled = disabled;
	wl_list_insert(&config->heads, &ch->link);

	if (id != 0) {
		ch->resource = wl_resource_create(
		    client, &zwlr_output_configuration_head_v1_interface,
		    wl_resource_get_version(config_resource), id);
		if (!ch->resource) {
			wl_list_remove(&ch->link);
			free(ch);
			wl_client_post_no_memory(client);
			return NULL;
		}
		wl_resource_set_implementation(ch->resource, &config_head_impl, ch,
		                               config_head_resource_destroy);
	}

	return ch;
}

static void
config_enable_head(struct wl_client *client, struct wl_resource *resource,
                   uint32_t id, struct wl_resource *head)
{
	struct output_configuration *config = wl_resource_get_user_data(resource);

	if (config) {
		config_add_head(config, client, resource, head, id, false);
	}
}

static void
config_disable_head(struct wl_client *client, struct wl_resource *resource,
                    struct wl_resource *head)
{
	struct output_configuration *config = wl_resource_get_user_data(resource);

	if (config) {
		config_add_head(config, client, resource, head, 0, true);
	}
}

/*
 * Whether the configuration can be honoured. Every screen must be named --
 * omitting one is a protocol error -- nothing may be disabled, and nothing may
 * ask for a property swc cannot change.
 */
static bool
config_is_valid(struct output_configuration *config)
{
	struct output_config_head *ch;
	struct screen *screen;

	wl_list_for_each (ch, &config->heads, link) {
		if (ch->disabled || ch->unsupported) {
			return false;
		}
	}

	wl_list_for_each (screen, &swc.screens, link) {
		if (!config_head_get(config, screen)) {
			return false;
		}
	}

	return true;
}

static void
config_finish(struct output_configuration *config, bool apply)
{
	struct output_config_head *ch;

	if (config->serial != current_serial) {
		/* The layout changed under the client; its request describes a world
		 * that no longer exists. */
		zwlr_output_configuration_v1_send_cancelled(config->resource);
		return;
	}

	if (!config_is_valid(config)) {
		zwlr_output_configuration_v1_send_failed(config->resource);
		return;
	}

	if (apply) {
		wl_list_for_each (ch, &config->heads, link) {
			if (ch->position_set) {
				swc_screen_set_position(&ch->screen->base, ch->x, ch->y);
			}
		}
	}

	zwlr_output_configuration_v1_send_succeeded(config->resource);
}

static void
config_apply(struct wl_client *client, struct wl_resource *resource)
{
	struct output_configuration *config = wl_resource_get_user_data(resource);

	(void)client;
	if (!config) {
		return;
	}
	if (config->used) {
		wl_resource_post_error(resource,
		                       ZWLR_OUTPUT_CONFIGURATION_V1_ERROR_ALREADY_USED,
		                       "configuration already applied or tested");
		return;
	}
	config->used = true;
	config_finish(config, true);
}

static void
config_test(struct wl_client *client, struct wl_resource *resource)
{
	struct output_configuration *config = wl_resource_get_user_data(resource);

	(void)client;
	if (!config) {
		return;
	}
	if (config->used) {
		wl_resource_post_error(resource,
		                       ZWLR_OUTPUT_CONFIGURATION_V1_ERROR_ALREADY_USED,
		                       "configuration already applied or tested");
		return;
	}
	config->used = true;
	config_finish(config, false);
}

static void
config_destroy_request(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct zwlr_output_configuration_v1_interface config_impl = {
    .enable_head = config_enable_head,
    .disable_head = config_disable_head,
    .apply = config_apply,
    .test = config_test,
    .destroy = config_destroy_request,
};

static void
config_resource_destroy(struct wl_resource *resource)
{
	struct output_configuration *config = wl_resource_get_user_data(resource);
	struct output_config_head *ch, *tmp;

	if (!config) {
		return;
	}
	wl_list_for_each_safe (ch, tmp, &config->heads, link) {
		if (ch->resource) {
			wl_resource_set_user_data(ch->resource, NULL);
		}
		wl_list_remove(&ch->link);
		free(ch);
	}
	free(config);
}

/* ----------------------------------------------------------------- manager */

static void
manager_create_configuration(struct wl_client *client,
                             struct wl_resource *resource, uint32_t id,
                             uint32_t serial)
{
	struct output_configuration *config;

	if (!(config = calloc(1, sizeof(*config)))) {
		wl_client_post_no_memory(client);
		return;
	}
	config->serial = serial;
	wl_list_init(&config->heads);

	config->resource =
	    wl_resource_create(client, &zwlr_output_configuration_v1_interface,
	                       wl_resource_get_version(resource), id);
	if (!config->resource) {
		free(config);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(config->resource, &config_impl, config,
	                               config_resource_destroy);
}

static void
manager_stop(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	zwlr_output_manager_v1_send_finished(resource);
	wl_resource_destroy(resource);
}

static const struct zwlr_output_manager_v1_interface manager_impl = {
    .create_configuration = manager_create_configuration,
    .stop = manager_stop,
};

static void
manager_resource_destroy(struct wl_resource *resource)
{
	struct output_manager *manager = wl_resource_get_user_data(resource);
	struct output_head *head, *tmp;

	if (!manager) {
		return;
	}
	wl_list_for_each_safe (head, tmp, &manager->heads, link) {
		wl_resource_set_user_data(head->resource, NULL);
		wl_list_remove(&head->link);
		free(head);
	}
	wl_list_remove(&manager->link);
	free(manager);
}

static void
bind_output_manager(struct wl_client *client, void *data, uint32_t version,
                    uint32_t id)
{
	struct output_manager *manager;

	(void)data;
	if (!(manager = calloc(1, sizeof(*manager)))) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_list_init(&manager->heads);

	manager->resource = wl_resource_create(
	    client, &zwlr_output_manager_v1_interface, version, id);
	if (!manager->resource) {
		free(manager);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(manager->resource, &manager_impl, manager,
	                               manager_resource_destroy);
	wl_list_insert(&managers, &manager->link);

	manager_send_all(manager);
}

void
output_management_update(void)
{
	struct output_manager *manager;
	struct output_head *head;

	++current_serial;

	wl_list_for_each (manager, &managers, link) {
		/*
		 * Only the position can have changed -- nothing else here is mutable --
		 * so re-send that rather than tearing every head down and rebuilding
		 * it. The protocol asks for the changes, not the whole state.
		 */
		wl_list_for_each (head, &manager->heads, link) {
			zwlr_output_head_v1_send_position(head->resource,
			                                  head->screen->base.geometry.x,
			                                  head->screen->base.geometry.y);
		}
		zwlr_output_manager_v1_send_done(manager->resource, current_serial);
	}
}

struct wl_global *
output_management_create(struct wl_display *display)
{
	wl_list_init(&managers);

	return wl_global_create(display, &zwlr_output_manager_v1_interface,
	                        OUTPUT_MANAGEMENT_VERSION, NULL,
	                        &bind_output_manager);
}
