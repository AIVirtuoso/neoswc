/* swc: libswc/subsurface.c
 *
 * Copyright (c) 2015-2019 Michael Forney
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

#include "subsurface.h"
#include "compositor.h"
#include "surface.h"
#include "util.h"
#include "view.h"

#include <stdlib.h>
#include <wayland-server.h>

static void
subsurface_update_position(struct subsurface *subsurface)
{
	struct compositor_view *parent_view;
	struct compositor_view *view;

	if (!subsurface->surface || !subsurface->parent)
		return;

	view = compositor_view(subsurface->surface->view);
	parent_view = compositor_view(subsurface->parent->view);
	if (!view || !parent_view)
		return;

	view_move(&view->base,
	          parent_view->base.geometry.x + subsurface->x - parent_view->buffer_offset_x,
	          parent_view->base.geometry.y + subsurface->y - parent_view->buffer_offset_y);
}

static void
handle_parent_view_move(struct view_handler *handler)
{
	struct subsurface *subsurface = wl_container_of(handler, subsurface, parent_view_handler);
	subsurface_update_position(subsurface);
}

static const struct view_handler_impl parent_view_handler_impl = {
	.move = handle_parent_view_move,
};

static void
handle_surface_destroy(struct wl_listener *listener, void *data)
{
	(void)data;
	struct subsurface *subsurface = wl_container_of(listener, subsurface, surface_destroy_listener);
	if (subsurface->resource)
		wl_resource_destroy(subsurface->resource);
}

static void
handle_parent_destroy(struct wl_listener *listener, void *data)
{
	(void)data;
	struct subsurface *subsurface = wl_container_of(listener, subsurface, parent_destroy_listener);
	struct compositor_view *view = NULL;

	if (subsurface->surface && subsurface->surface->view)
		view = compositor_view(subsurface->surface->view);

	if (view) {
		view->parent = NULL;
		compositor_view_hide(view);
	}

	if (!wl_list_empty(&subsurface->parent_view_handler.link)) {
		wl_list_remove(&subsurface->parent_view_handler.link);
		wl_list_init(&subsurface->parent_view_handler.link);
	}

	if (!wl_list_empty(&subsurface->link)) {
		wl_list_remove(&subsurface->link);
		wl_list_init(&subsurface->link);
	}

	subsurface->parent = NULL;
}

static void
set_position(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y)
{
	(void)client;
	struct subsurface *subsurface = wl_resource_get_user_data(resource);

	subsurface->x = x;
	subsurface->y = y;
	subsurface_update_position(subsurface);
}

static void
place_above(struct wl_client *client, struct wl_resource *resource, struct wl_resource *sibling_resource)
{
	(void)client;
	struct subsurface *subsurface = wl_resource_get_user_data(resource);
	struct surface *sibling_surface = wl_resource_get_user_data(sibling_resource);
	struct compositor_view *view = compositor_view(subsurface->surface->view);
	struct compositor_view *sibling_view = compositor_view(sibling_surface->view);

	if (!view || !sibling_view || view == sibling_view)
		return;

	if (view->parent != sibling_view->parent)
		return;

	wl_list_remove(&view->link);
	wl_list_insert(sibling_view->link.prev, &view->link);
}

static void
place_below(struct wl_client *client, struct wl_resource *resource, struct wl_resource *sibling_resource)
{
	(void)client;
	struct subsurface *subsurface = wl_resource_get_user_data(resource);
	struct surface *sibling_surface = wl_resource_get_user_data(sibling_resource);
	struct compositor_view *view = compositor_view(subsurface->surface->view);
	struct compositor_view *sibling_view = compositor_view(sibling_surface->view);

	if (!view || !sibling_view || view == sibling_view)
		return;

	if (view->parent != sibling_view->parent)
		return;

	wl_list_remove(&view->link);
	wl_list_insert(&sibling_view->link, &view->link);
}

static void
set_sync(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	struct subsurface *subsurface = wl_resource_get_user_data(resource);
	subsurface->sync = true;
}

static void
set_desync(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	struct subsurface *subsurface = wl_resource_get_user_data(resource);
	subsurface->sync = false;
}

static const struct wl_subsurface_interface subsurface_impl = {
	.destroy = destroy_resource,
	.set_position = set_position,
	.place_above = place_above,
	.place_below = place_below,
	.set_sync = set_sync,
	.set_desync = set_desync,
};

static void
subsurface_destroy(struct wl_resource *resource)
{
	struct subsurface *subsurface = wl_resource_get_user_data(resource);

	if (subsurface->surface) {
		if (subsurface->surface->subsurface == subsurface)
			subsurface->surface->subsurface = NULL;
	}

	if (!wl_list_empty(&subsurface->parent_destroy_listener.link)) {
		wl_list_remove(&subsurface->parent_destroy_listener.link);
		wl_list_init(&subsurface->parent_destroy_listener.link);
	}
	if (!wl_list_empty(&subsurface->surface_destroy_listener.link)) {
		wl_list_remove(&subsurface->surface_destroy_listener.link);
		wl_list_init(&subsurface->surface_destroy_listener.link);
	}

	if (!wl_list_empty(&subsurface->parent_view_handler.link)) {
		wl_list_remove(&subsurface->parent_view_handler.link);
		wl_list_init(&subsurface->parent_view_handler.link);
	}

	if (!wl_list_empty(&subsurface->link)) {
		wl_list_remove(&subsurface->link);
		wl_list_init(&subsurface->link);
	}

	if (subsurface->surface && subsurface->surface->view) {
		struct compositor_view *view = compositor_view(subsurface->surface->view);
		if (view && !view->window)
			compositor_view_destroy(view);
	}

	free(subsurface);
}

struct subsurface *
subsurface_new(struct wl_client *client, uint32_t version, uint32_t id,
               struct surface *surface, struct surface *parent)
{
	struct subsurface *subsurface;
	struct compositor_view *parent_view;
	struct compositor_view *view;

	if (!(subsurface = malloc(sizeof(*subsurface))))
		goto error0;

	subsurface->resource = wl_resource_create(client, &wl_subsurface_interface, version, id);

	if (!subsurface->resource)
		goto error1;

	wl_resource_set_implementation(subsurface->resource, &subsurface_impl, subsurface, &subsurface_destroy);

	subsurface->surface = surface;
	subsurface->parent = parent;
	subsurface->x = 0;
	subsurface->y = 0;
	subsurface->sync = true;
	subsurface->pending = false;

	subsurface->parent_view_handler.impl = &parent_view_handler_impl;
	wl_list_init(&subsurface->parent_view_handler.link);
	wl_list_init(&subsurface->surface_destroy_listener.link);
	wl_list_init(&subsurface->parent_destroy_listener.link);
	wl_list_init(&subsurface->link);

	if (!surface->view)
		compositor_create_view(surface);
	if (!parent->view)
		compositor_create_view(parent);

	parent_view = compositor_view(parent->view);
	view = compositor_view(surface->view);
	if (!parent_view || !view)
		goto error2;

	compositor_view_set_parent(view, parent_view);
	wl_list_remove(&view->link);
	wl_list_insert(parent_view->link.prev, &view->link);

	wl_list_insert(&parent_view->base.handlers, &subsurface->parent_view_handler.link);
	subsurface_update_position(subsurface);
	wl_list_insert(&parent->subsurfaces, &subsurface->link);

	subsurface->surface_destroy_listener.notify = handle_surface_destroy;
	wl_resource_add_destroy_listener(surface->resource, &subsurface->surface_destroy_listener);
	subsurface->parent_destroy_listener.notify = handle_parent_destroy;
	wl_resource_add_destroy_listener(parent->resource, &subsurface->parent_destroy_listener);

	return subsurface;

error2:
	wl_resource_destroy(subsurface->resource);
	return NULL;
error1:
	free(subsurface);
error0:
	return NULL;
}
