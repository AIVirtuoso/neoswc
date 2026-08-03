/* swc: libswc/window.h
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

#ifndef SWC_WINDOW_H
#define SWC_WINDOW_H

#include "pointer.h"
#include "swc.h"
#include "transaction.h"

#include <stdint.h>
#include <wayland-server.h>

struct window_pointer_interaction {
	bool active;
	uint32_t serial;
	struct pointer_handler handler, *original_handler;
};

enum window_mode {
	WINDOW_MODE_STACKED,
	WINDOW_MODE_TILED,
	WINDOW_MODE_FULLSCREEN,
};

struct window {
	struct swc_window base;
	const struct window_impl *impl;
	const struct swc_window_handler *handler;
	void *handler_data;

	struct compositor_view *view;
	struct view_handler view_handler;
	bool managed;
	unsigned mode;

	struct {
		struct swc_rectangle geom;
		unsigned mode;
	} prev;

	struct {
		struct window_pointer_interaction interaction;
		struct {
			int32_t x, y;
		} offset;

		bool pending;
		int32_t x, y;
		uint32_t last_time;
	} move;

	struct {
		struct window_pointer_interaction interaction;
		struct {
			int32_t x, y;
		} offset;
		uint32_t edges;
		uint32_t last_time;
	} resize;

	struct {
		bool pending, acknowledged;
		uint32_t width, height;
		/*
		 * Identifies the most recent configure this window was asked to
		 * make. Owned by window.c and independent of any shell's own
		 * serial, so all three window_impls can share one ack path.
		 */
		uint32_t serial;
	} configure;

	/*
	 * The client's zxdg_toplevel_decoration_v1, if it created one. Owned by
	 * xdg_decoration.c, which clears it on destroy.
	 */
	struct wl_resource *decoration;

	/* Non-NULL while enrolled in an open cohort; see swc_transaction_begin. */
	struct transaction *transaction;
	/* Link into the open cohort, then into the one awaiting present. */
	struct wl_list transaction_link;
	/* Whether this window responded, carried past the transaction's lifetime. */
	bool transaction_acked;
};

struct window_impl {
	void (*move)(struct window *window, int32_t x, int32_t y);
	void (*configure)(struct window *window, uint32_t width, uint32_t height);
	void (*focus)(struct window *window);
	void (*unfocus)(struct window *window);
	void (*close)(struct window *window);
	void (*set_mode)(struct window *window, enum window_mode mode);
};

/* The transaction API is public; see swc_transaction_begin() in swc.h. */

/*
 * Record that a window has acknowledged its current configure. Shells with a
 * real round trip (xdg-shell) call this from their ack handler; shells without
 * one set configure.acknowledged inside their own configure() and are enrolled
 * as already-acked.
 */
void
window_ack_configure(struct window *window);

extern struct wl_listener window_enter_listener;

bool
window_initialize(struct window *window, const struct window_impl *impl,
                  struct surface *surface);
void
window_finalize(struct window *window);
void
window_manage(struct window *window);
void
window_unmanage(struct window *window);
void
window_request_maximize(struct window *window);
void
window_request_unmaximize(struct window *window);
void
window_request_minimize(struct window *window);
void
window_request_window_menu(struct window *window, int32_t x, int32_t y);
void
window_set_title(struct window *window, const char *title, size_t length);
void
window_set_app_id(struct window *window, const char *app_id);
void
window_set_parent(struct window *window, struct window *parent);
void
window_begin_move(struct window *window, struct button *button);
void
window_begin_resize(struct window *window, uint32_t edges,
                    struct button *button);

#endif
