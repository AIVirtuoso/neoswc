/* swc: libswc/window.c
 *
 * Copyright (c) 2013 Michael Forney
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

#include "window.h"
#include "compositor.h"
#include "event.h"
#include "internal.h"
#include "keyboard.h"
#include "seat.h"
#include "surface.h"
#include "swc.h"
#include "util.h"
#include "view.h"

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define INTERNAL(w) ((struct window *)(w))

static const uint32_t def_motion_throttle_ms = 16;

static const struct swc_window_handler null_handler;

/*
 * The single open cohort, if any. There is one because the protocol has one
 * manage sequence in flight at a time; a second begin() while one is open is a
 * caller bug rather than something to queue.
 */
static struct transaction *window_transaction;
static struct wl_list transaction_windows;
static void (*transaction_done_cb)(bool timed_out, void *data);
static void *transaction_done_data;

static bool
should_throttle_motion(uint32_t throttle_ms, uint32_t *last_time, uint32_t time)
{
	if (!throttle_ms) {
		return false;
	}

	if (*last_time && time - *last_time < throttle_ms) {
		return true;
	}

	*last_time = time;
	return false;
}

static uint32_t
clamp_dimension(int32_t value, uint32_t min, uint32_t max)
{
	if (value < 0) {
		value = 0;
	}

	if (min && value < min) {
		value = min;
	}

	if (max) {
		if (min && max < min) {
			max = min;
		}

		if (value > max) {
			value = max;
		}
	}

	if (value > UINT32_MAX) {
		value = UINT32_MAX;
	}

	return value;
}

static void
clamp_window_size(const struct window *window, uint32_t *width,
                  uint32_t *height)
{
	*width =
	    clamp_dimension(*width, window->base.min_width, window->base.max_width);
	*height = clamp_dimension(*height, window->base.min_height,
	                          window->base.max_height);
}

static void
handle_window_enter(struct wl_listener *listener, void *data)
{
	struct event *event = data;
	struct input_focus_event_data *event_data = event->data;
	struct window *window;

	if (event->type != INPUT_FOCUS_EVENT_CHANGED) {
		return;
	}

	if (!event_data->new || !(window = event_data->new->window)) {
		return;
	}

	if (window->handler->entered) {
		window->handler->entered(window->handler_data);
	}
}

struct wl_listener window_enter_listener = {
    .notify = handle_window_enter,
};

static void
begin_interaction(struct window_pointer_interaction *interaction,
                  struct button *button)
{
	if (button) {
		/* Store the serial of the button press so we are able to cancel the
		 * interaction if the window changes from stacked mode. */
		interaction->serial = button->press.serial;
		interaction->original_handler = button->handler;
		button->handler = &interaction->handler;
	} else {
		interaction->original_handler = NULL;
	}

	interaction->active = true;
	wl_list_insert(&swc.seat->pointer->handlers, &interaction->handler.link);
}

static void
end_interaction(struct window_pointer_interaction *interaction,
                struct button *button)
{
	if (!interaction->active) {
		return;
	}

	if (interaction->original_handler) {
		if (!button) {
			button = pointer_get_button(swc.seat->pointer, interaction->serial);

			if (!button) {
				WARNING("No button with serial %u\n", interaction->serial);
				goto remove;
			}
		}

		interaction->original_handler->button(interaction->original_handler,
		                                      get_time(), button,
		                                      WL_POINTER_BUTTON_STATE_RELEASED);
	}

remove:
	interaction->active = false;
	wl_list_remove(&interaction->handler.link);
}

static void
flush(struct window *window)
{
	if (window->move.pending) {
		if (window->impl->move) {
			window->impl->move(window, window->move.x, window->move.y);
		}

		view_move(&window->view->base, window->move.x, window->move.y);
		window->move.pending = false;
	}
}

/*
 * Withdraw a window from the open cohort. Unlinking before notifying the
 * barrier matters: transaction_remove() can complete the cohort, and the
 * completion handler walks this same list.
 */
static void
leave_transaction(struct window *window)
{
	struct transaction *transaction = window->transaction;

	if (!transaction) {
		return;
	}

	window->transaction = NULL;
	wl_list_remove(&window->transaction_link);
	wl_list_init(&window->transaction_link);
	surface_release_render(window->view->surface);
	transaction_remove(transaction, window);
}

/*
 * Enrol a window in the open cohort. `awaiting_ack` says whether this window
 * owes a response: false means it is enrolled purely so its move lands with
 * everyone else's, and it settles immediately rather than holding the barrier.
 *
 * Deciding this at the call site rather than inferring it here is deliberate.
 * configure.pending is only ever set for tiled windows, so it cannot be used
 * to tell "no round trip needed" from "stacked window mid-configure".
 */
static void
join_transaction(struct window *window, bool awaiting_ack)
{
	if (!window_transaction || window->transaction) {
		return;
	}

	if (!transaction_add(window_transaction, window, window->configure.serial)) {
		return;
	}

	window->transaction = window_transaction;
	wl_list_insert(&transaction_windows, &window->transaction_link);

	/*
	 * Hold the window's commits for the duration of the cohort. Without
	 * this the barrier only synchronises moves: a window that acks and
	 * commits early would paint its new size at its old position while its
	 * neighbours are still being configured.
	 */
	surface_hold_render(window->view->surface);

	if (!awaiting_ack) {
		transaction_ack(window_transaction, window, window->configure.serial);
	}
}

static void
transaction_done(struct transaction *transaction, bool timed_out, void *data)
{
	struct window *window, *tmp;
	void (*done)(bool timed_out, void *data);
	void *data_ptr;

	(void)data;

	wl_list_for_each_safe (window, tmp, &transaction_windows,
	                       transaction_link) {
		bool acked = transaction_acked(transaction, window);

		wl_list_remove(&window->transaction_link);
		wl_list_init(&window->transaction_link);
		window->transaction = NULL;

		if (!acked) {
			/*
			 * A straggler. Leave move.pending and configure.pending set so
			 * the per-window path picks it up when its buffer finally
			 * arrives -- the protocol expects its dimensions in a later
			 * render sequence, not never. Release the hold regardless, or
			 * an unresponsive window would stay frozen forever.
			 */
			surface_release_render(window->view->surface);
			continue;
		}

		flush(window);
		window->configure.pending = false;
		/*
		 * Move first, then promote the buffer. Both only queue damage; the
		 * repaint is an idle callback, so everything dispatched before we
		 * return to the event loop composites into a single frame. That is
		 * what makes the relayout atomic on screen rather than merely
		 * synchronised in bookkeeping.
		 */
		surface_release_render(window->view->surface);
	}

	window_transaction = NULL;
	transaction_destroy(transaction);

	/* Clear before dispatching: the callback may begin the next transaction. */
	done = transaction_done_cb;
	data_ptr = transaction_done_data;
	transaction_done_cb = NULL;
	transaction_done_data = NULL;

	if (done) {
		done(timed_out, data_ptr);
	}
}

static const struct transaction_handler barrier_handler = {
    .complete = transaction_done,
};

EXPORT bool
swc_transaction_begin(void)
{
	if (window_transaction) {
		return false;
	}

	window_transaction =
	    transaction_create(swc.event_loop, &barrier_handler, NULL);
	if (!window_transaction) {
		return false;
	}

	wl_list_init(&transaction_windows);
	return true;
}

EXPORT void
swc_transaction_commit(uint32_t timeout_ms,
                       void (*done)(bool timed_out, void *data), void *data)
{
	if (!window_transaction) {
		return;
	}

	transaction_done_cb = done;
	transaction_done_data = data;
	/* May complete, and tear everything down, before this returns. */
	transaction_commit(window_transaction, timeout_ms);
}

EXPORT bool
swc_transaction_active(void)
{
	return window_transaction != NULL;
}

void
window_ack_configure(struct window *window)
{
	window->configure.acknowledged = true;

	if (window->transaction) {
		transaction_ack(window->transaction, window, window->configure.serial);
	}
}

EXPORT void
swc_window_set_handler(struct swc_window *base,
                       const struct swc_window_handler *handler, void *data)
{
	struct window *window = INTERNAL(base);

	window->handler = handler;
	window->handler_data = data;
}

EXPORT void
swc_window_close(struct swc_window *base)
{
	struct window *window = INTERNAL(base);

	if (window->impl->close) {
		window->impl->close(window);
	}
}

EXPORT void
swc_window_show(struct swc_window *window)
{
	compositor_view_show(INTERNAL(window)->view);
}

EXPORT void
swc_window_hide(struct swc_window *window)
{
	compositor_view_hide(INTERNAL(window)->view);
}

EXPORT void
swc_window_focus(struct swc_window *base)
{
	struct window *window = INTERNAL(base);
	struct compositor_view *new = window ? window->view : NULL,
	                       *old = swc.seat->keyboard->focus.view;

	if (new == old) {
		return;
	}

	/* Focus the new window before unfocusing the old one in case both are X11
	 * windows so the xwl_window implementation can handle this transition
	 * correctly. */
	if (window && window->impl->focus) {
		window->impl->focus(window);
	}
	if (old && old->window && old->window->impl->unfocus) {
		old->window->impl->unfocus(old->window);
	}

	keyboard_set_focus(swc.seat->keyboard, new);
}

EXPORT void
swc_window_set_stacked(struct swc_window *base)
{
	struct window *window = INTERNAL(base);

	/*
	 * Leave the flush to the cohort if this window is enrolled. Applying it
	 * here would let a mode change jump the barrier while the window's
	 * neighbours are still waiting.
	 */
	if (!window->transaction) {
		flush(window);
	}
	window->configure.pending = false;
	window->configure.width = 0;
	window->configure.height = 0;
	if (window->impl->set_mode) {
		window->impl->set_mode(window, WINDOW_MODE_STACKED);
	}
	window->mode = WINDOW_MODE_STACKED;
}

EXPORT void
swc_window_set_tiled(struct swc_window *base)
{
	struct window *window = INTERNAL(base);

	end_interaction(&window->move.interaction, NULL);
	end_interaction(&window->resize.interaction, NULL);
	if (window->impl->set_mode) {
		window->impl->set_mode(window, WINDOW_MODE_TILED);
	}
	window->mode = WINDOW_MODE_TILED;
}

EXPORT void
swc_window_set_fullscreen(struct swc_window *base, struct swc_screen *screen)
{
	struct window *window = INTERNAL(base);

	struct swc_rectangle geom;
	swc_window_get_geometry(base, &geom);

	if (window->mode != WINDOW_MODE_FULLSCREEN) {
		window->prev.geom = geom;
		window->prev.mode = window->mode;
		swc_window_set_geometry(base, &screen->usable_geometry);

		if (window->impl->set_mode) {
			window->impl->set_mode(window, WINDOW_MODE_FULLSCREEN);
		}
		window->mode = WINDOW_MODE_FULLSCREEN;
	}

	else {
		swc_window_set_geometry(base, &window->prev.geom);
		window->mode = window->prev.mode;
	}
}

EXPORT void
swc_window_set_position(struct swc_window *base, int32_t x, int32_t y)
{
	struct window *window = INTERNAL(base);
	struct swc_rectangle *geometry = &window->view->base.geometry;

	if (x == geometry->x && y == geometry->y) {
		window->move.pending = false;
		return;
	}

	window->move.x = x;
	window->move.y = y;
	window->move.pending = true;

	/*
	 * Inside a cohort the move is held until every member has responded,
	 * even when this window has no configure outstanding -- a pure
	 * reposition must still land in the same frame as its neighbours.
	 */
	if (window_transaction) {
		join_transaction(window, window->configure.pending &&
		                             !window->configure.acknowledged);
		return;
	}

	/* If we don't have a configure pending, perform the move now. */
	if (!window->configure.pending) {
		flush(window);
	}
}

EXPORT void
swc_window_set_size(struct swc_window *base, uint32_t width, uint32_t height)
{
	struct window *window = INTERNAL(base);
	struct swc_rectangle *geom = &window->view->base.geometry;

	clamp_window_size(window, &width, &height);

	if ((window->configure.pending && width == window->configure.width &&
	     height == window->configure.height) ||
	    (!window->configure.pending && width == geom->width &&
	     height == geom->height)) {
		return;
	}

	/*
	 * Bump before dispatching: shells that acknowledge synchronously do so
	 * from inside configure(), and must settle against the new serial.
	 */
	++window->configure.serial;
	window->configure.acknowledged = false;
	window->impl->configure(window, width, height);

	if (window->mode == WINDOW_MODE_TILED) {
		window->configure.width = width;
		window->configure.height = height;
		window->configure.pending = true;
	}

	/*
	 * wl_shell and Xwayland acknowledge inside configure(), so by now the
	 * response has already happened and the window settles on enrolment.
	 * xdg-shell leaves it outstanding and the cohort waits.
	 */
	if (window_transaction) {
		join_transaction(window, !window->configure.acknowledged);
	}
}

EXPORT void
swc_window_set_geometry(struct swc_window *window,
                        const struct swc_rectangle *geometry)
{
	swc_window_set_size(window, geometry->width, geometry->height);
	swc_window_set_position(window, geometry->x, geometry->y);
}

EXPORT bool
swc_window_get_geometry(const struct swc_window *base,
                        struct swc_rectangle *geometry)
{
	struct window *window = INTERNAL((struct swc_window *)base);

	if (!window || !geometry) {
		return false;
	}

	*geometry = window->view->base.geometry;
	return true;
}

EXPORT void
swc_window_set_border(struct swc_window *window, uint32_t inner_border_color,
                      uint32_t inner_border_width, uint32_t outer_border_color,
                      uint32_t outer_border_width)
{
	struct compositor_view *view = INTERNAL(window)->view;

	compositor_view_set_border_color(view, outer_border_color,
	                                 inner_border_color);
	compositor_view_set_border_width(view, outer_border_width,
	                                 inner_border_width);
}

EXPORT void
swc_window_set_decor(struct swc_window *window, const struct swc_decor *decor)
{
	struct compositor_view *view = INTERNAL(window)->view;

	compositor_view_set_decor(view, decor);
}

EXPORT void
swc_window_begin_move(struct swc_window *window)
{
	window_begin_move(INTERNAL(window), NULL);
}

EXPORT void
swc_window_end_move(struct swc_window *window)
{
	end_interaction(&INTERNAL(window)->move.interaction, NULL);
}

EXPORT void
swc_window_begin_resize(struct swc_window *window, uint32_t edges)
{
	window_begin_resize(INTERNAL(window), edges, NULL);
}

EXPORT void
swc_window_end_resize(struct swc_window *window)
{
	end_interaction(&INTERNAL(window)->resize.interaction, NULL);
}

static bool
move_motion(struct pointer_handler *handler, uint32_t time, wl_fixed_t fx,
            wl_fixed_t fy)
{
	struct window *window =
	    wl_container_of(handler, window, move.interaction.handler);

	if (should_throttle_motion(window->base.motion_throttle_ms,
	                           &window->move.last_time, time)) {
		return true;
	}

	int32_t x = wl_fixed_to_int(fx) + window->move.offset.x,
	        y = wl_fixed_to_int(fy) + window->move.offset.y;

	view_move(&window->view->base, x, y);
	return true;
}

static bool
resize_motion(struct pointer_handler *handler, uint32_t time, wl_fixed_t fx,
              wl_fixed_t fy)
{
	struct window *window =
	    wl_container_of(handler, window, resize.interaction.handler);
	const struct swc_rectangle *geometry = &window->view->base.geometry;
	uint32_t width = geometry->width, height = geometry->height;

	if (should_throttle_motion(window->base.motion_throttle_ms,
	                           &window->resize.last_time, time)) {
		return true;
	}

	if (window->resize.edges & SWC_WINDOW_EDGE_LEFT) {
		width -= wl_fixed_to_int(fx) + window->resize.offset.x - geometry->x;
	} else if (window->resize.edges & SWC_WINDOW_EDGE_RIGHT) {
		width = wl_fixed_to_int(fx) + window->resize.offset.x - geometry->x;
	}

	if (window->resize.edges & SWC_WINDOW_EDGE_TOP) {
		height -= wl_fixed_to_int(fy) + window->resize.offset.y - geometry->y;
	} else if (window->resize.edges & SWC_WINDOW_EDGE_BOTTOM) {
		height = wl_fixed_to_int(fy) + window->resize.offset.y - geometry->y;
	}

	clamp_window_size(window, &width, &height);
	window->impl->configure(window, width, height);

	return true;
}

static bool
handle_button(struct pointer_handler *handler, uint32_t time,
              struct button *button, uint32_t state)
{
	struct window_pointer_interaction *interaction =
	    wl_container_of(handler, interaction, handler);

	if (state != WL_POINTER_BUTTON_STATE_RELEASED ||
	    !interaction->original_handler) {
		return false;
	}

	end_interaction(interaction, button);
	return true;
}

static void
handle_attach(struct view_handler *handler)
{
	struct window *window = wl_container_of(handler, window, view_handler);

	/*
	 * Enrolled windows are flushed by the cohort, not by their own buffer
	 * arriving. Applying here would defeat the barrier: it is exactly the
	 * piecewise behaviour the cohort exists to replace.
	 */
	if (window->transaction) {
		return;
	}

	if (window->configure.acknowledged) {
		flush(window);
	}
	window->configure.pending = false;
}

static void
handle_resize(struct view_handler *handler, uint32_t old_width,
              uint32_t old_height)
{
	struct window *window = wl_container_of(handler, window, view_handler);

	if (window->resize.interaction.active &&
	    window->resize.edges & (SWC_WINDOW_EDGE_TOP | SWC_WINDOW_EDGE_LEFT)) {
		const struct swc_rectangle *geometry = &window->view->base.geometry;
		int32_t x = geometry->x, y = geometry->y;

		if (window->resize.edges & SWC_WINDOW_EDGE_LEFT) {
			x += old_width - geometry->width;
		}
		if (window->resize.edges & SWC_WINDOW_EDGE_TOP) {
			y += old_height - geometry->height;
		}

		view_move(&window->view->base, x, y);
	}
}

static const struct view_handler_impl view_handler_impl = {
    .attach = handle_attach,
    .resize = handle_resize,
};

bool
window_initialize(struct window *window, const struct window_impl *impl,
                  struct surface *surface)
{
	DEBUG("Initializing window, %p\n", window);

	window->base.title = NULL;
	window->base.app_id = NULL;
	window->base.parent = NULL;

	if (surface->view) {
		window->view = compositor_view(surface->view);
		if (!window->view || window->view->window) {
			return false;
		}
	} else {
		if (!(window->view = compositor_create_view(surface))) {
			return false;
		}
	}

	window->impl = impl;
	window->handler = &null_handler;
	window->view_handler.impl = &view_handler_impl;
	window->view->window = window;
	window->base.motion_throttle_ms = def_motion_throttle_ms;
	window->base.min_width = 0;
	window->base.min_height = 0;
	window->base.max_width = 0;
	window->base.max_height = 0;
	window->managed = false;
	window->mode = WINDOW_MODE_STACKED;
	window->move.pending = false;
	window->move.last_time = 0;
	window->move.interaction.active = false;
	window->move.interaction.handler = (struct pointer_handler){
	    .motion = move_motion,
	    .button = handle_button,
	};
	window->configure.pending = false;
	window->configure.acknowledged = false;
	window->configure.width = 0;
	window->configure.height = 0;
	window->configure.serial = 0;
	window->transaction = NULL;
	wl_list_init(&window->transaction_link);
	window->resize.interaction.active = false;
	window->resize.interaction.handler = (struct pointer_handler){
	    .motion = resize_motion,
		.button = handle_button,
	};
	window->resize.last_time = 0;

	wl_list_insert(&window->view->base.handlers, &window->view_handler.link);

	return true;
}

void
window_finalize(struct window *window)
{
	DEBUG("Finalizing window, %p\n", window);

	/*
	 * A window closing mid-cohort must withdraw, or the barrier waits on an
	 * ack that can never arrive and stalls every other window until timeout.
	 */
	leave_transaction(window);
	window_unmanage(window);
	compositor_view_destroy(window->view);
	free(window->base.title);
	free(window->base.app_id);
}

void
window_manage(struct window *window)
{
	if (window->managed) {
		return;
	}

	swc.manager->new_window(&window->base);
	window->managed = true;
}

void
window_unmanage(struct window *window)
{
	if (!window->managed) {
		return;
	}

	if (window->handler->destroy) {
		window->handler->destroy(window->handler_data);
	}
	window->handler = &null_handler;
	window->managed = false;
}

void
window_set_title(struct window *window, const char *title, size_t length)
{
	free(window->base.title);
	window->base.title = strndup(title, length);

	if (window->handler->title_changed) {
		window->handler->title_changed(window->handler_data);
	}
}

void
window_set_app_id(struct window *window, const char *app_id)
{
	free(window->base.app_id);
	window->base.app_id = strdup(app_id);

	if (window->handler->app_id_changed) {
		window->handler->app_id_changed(window->handler_data);
	}
}

void
window_set_parent(struct window *window, struct window *parent)
{
	if (window->base.parent == &parent->base) {
		return;
	}

	compositor_view_set_parent(window->view, parent->view);
	window->base.parent = &parent->base;

	if (window->handler->parent_changed) {
		window->handler->parent_changed(window->handler_data);
	}
}

void
window_begin_move(struct window *window, struct button *button)
{
	if (window->mode != WINDOW_MODE_STACKED && window->handler->move) {
		window->handler->move(window->handler_data);
	}

	if (window->mode != WINDOW_MODE_STACKED ||
	    window->move.interaction.active) {
		return;
	}

	struct swc_rectangle *geometry = &window->view->base.geometry;
	int32_t px = wl_fixed_to_int(swc.seat->pointer->x),
	        py = wl_fixed_to_int(swc.seat->pointer->y);

	begin_interaction(&window->move.interaction, button);
	window->move.last_time = 0;
	window->move.offset.x = geometry->x - px;
	window->move.offset.y = geometry->y - py;
}

void
window_begin_resize(struct window *window, uint32_t edges,
                    struct button *button)
{
	if (window->mode != WINDOW_MODE_STACKED && window->handler->resize) {
		window->handler->resize(window->handler_data);
	}

	if (window->mode != WINDOW_MODE_STACKED ||
	    window->resize.interaction.active) {
		return;
	}

	struct swc_rectangle *geometry = &window->view->base.geometry;
	int32_t px = wl_fixed_to_int(swc.seat->pointer->x),
	        py = wl_fixed_to_int(swc.seat->pointer->y);

	begin_interaction(&window->resize.interaction, button);
	window->resize.last_time = 0;

	if (!edges) {
		edges |= (px < geometry->x + geometry->width / 2)
		             ? SWC_WINDOW_EDGE_LEFT
		             : SWC_WINDOW_EDGE_RIGHT;
		edges |= (py < geometry->y + geometry->height / 2)
		             ? SWC_WINDOW_EDGE_TOP
		             : SWC_WINDOW_EDGE_BOTTOM;
	}

	window->resize.offset.x =
	    geometry->x - px +
	    ((edges & SWC_WINDOW_EDGE_RIGHT) ? geometry->width : 0);
	window->resize.offset.y =
	    geometry->y - py +
	    ((edges & SWC_WINDOW_EDGE_BOTTOM) ? geometry->height : 0);
	window->resize.edges = edges;
}

EXPORT pid_t
swc_window_get_pid(struct swc_window *base)
{
	struct window *window = INTERNAL(base);
	struct surface *surface;
	struct wl_client *client;
	pid_t pid;
	uid_t uid;
	gid_t gid;

	if (!window || !window->view || !window->view->surface) {
		return 0;
	}

	surface = window->view->surface;
	if (!surface->resource) {
		return 0;
	}

	client = wl_resource_get_client(surface->resource);
	wl_client_get_credentials(client, &pid, &uid, &gid);

	return pid;
}
