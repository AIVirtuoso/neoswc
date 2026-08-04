/* swc: libswc/swc.h
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

#ifndef SWC_H
#define SWC_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct libinput_device;
struct wl_client;
struct wl_display;
struct wl_event_loop;
struct wl_resource;
struct wld_buffer;

/**
 * Get the current cursor position.
 *
 * The returned coordinates are in compositor-global space, in wl_fixed_t
 * (24.8) fixed-point units, but exposed as raw int32_t to avoid needing
 * wayland headers.
 *
 */
bool
swc_cursor_position(int32_t *x, int32_t *y);

/**
 * Send a pointer button event to the currently focused client.
 *
 * This is intended for window managers which intercept button events (for
 * example for mouse chords) but want normal clicks to still reach clients.
 */
void
swc_pointer_send_button(uint32_t time, uint32_t button, uint32_t state);

/**
 * Send a pointer axis event to the currently focused client.
 *
 * This is intended for window managers which intercept axis events (for
 * example for mouse chords) but want normal scrolling to still reach clients.
 *
 * value120 uses the wl_pointer "120 units" convention.
 */
void
swc_pointer_send_axis(uint32_t time, uint32_t axis, int32_t value120);

/* Cursor control (compositor-internal cursor) */
enum swc_cursor_kind {
	SWC_CURSOR_DEFAULT = 0,
	SWC_CURSOR_BOX = 1,
	SWC_CURSOR_CROSS = 2,
	SWC_CURSOR_SIGHT = 3,
	SWC_CURSOR_UP = 4,
	SWC_CURSOR_DOWN = 5,
};

enum swc_cursor_mode {
	/* Allow clients to set their own cursors (I-beam, resize, etc). */
	SWC_CURSOR_MODE_CLIENT = 0,
	/* Force compositor cursor; ignore client wl_pointer.set_cursor. */
	SWC_CURSOR_MODE_COMPOSITOR = 1,
};

/**
 * Override the compositor's internal cursor.
 *
 * this is intended for window managers to show mode cursors
 * (move/resize/select) like the ones in hevel If a client has set its own
 * cursor surface, swc may ignore the override.
 */
void
swc_set_cursor(enum swc_cursor_kind kind);

/**
 * Control whether client cursor surfaces are honored.
 */
void
swc_set_cursor_mode(enum swc_cursor_mode mode);

/**
 * set a custom argb8888 cursor image for a given kind
 *
 * `argb8888` is a pointer to `width*height` pixels in ARGB8888 order.
 * the caller has to keep the pixel memory alive for as long as it may be used
 */
void
swc_set_cursor_image(enum swc_cursor_kind kind, const uint32_t *argb8888,
                     uint32_t width, uint32_t height, int32_t hotspot_x,
                     int32_t hotspot_y);

void
swc_clear_cursor_image(enum swc_cursor_kind kind);

/**
 * draw [or update] a simple box overlay
 *
 * box is defined by two diagonally opposite corners in compositor-global
 * coordinates. this draws only the border. Call swc_overlay_clear() to remove
 * it
 */
void
swc_overlay_set_box(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                    uint32_t color, uint32_t border_width);

/**
 * Clear the current overlay, if any.
 */
void
swc_overlay_clear(void);

/**
 * Set the compositor zoom level.
 *
 * 1.0 = normal, >1.0 = zoomed in, <1.0 = zoomed out
 * Uses software (pixman) scaling.
 */
void
swc_set_zoom(float level);

/**
 * Get the current zoom level.
 */
float
swc_get_zoom(void);

/* Rectangles {{{ */

struct swc_rectangle {
	int32_t x, y;
	uint32_t width, height;
};

/* }}} */

/* Screens {{{ */

struct swc_screen_handler {
	/**
	 * Called when the screen is about to be destroyed.
	 *
	 * After this is called, the screen is no longer valid.
	 */
	void (*destroy)(void *data);

	/**
	 * Called when the total area of the screen has changed.
	 */
	void (*geometry_changed)(void *data);

	/**
	 * Called when the geometry of the screen available for laying out windows
	 * has changed.
	 *
	 * A window manager should respond by making sure all visible windows are
	 * within this area.
	 */
	void (*usable_geometry_changed)(void *data);

	/**
	 * Called when the pointer enters the screen.
	 */
	void (*entered)(void *data);
};

struct swc_screen {
	/**
	 * The total area of the screen.
	 */
	struct swc_rectangle geometry;

	/**
	 * The area of the screen available for placing windows.
	 */
	struct swc_rectangle usable_geometry;
};

/**
 * Set the handler associated with this screen.
 */
void
swc_screen_set_handler(struct swc_screen *screen,
                       const struct swc_screen_handler *handler, void *data);

/**
 * Move the screen to the given position in the global coordinate space.
 *
 * swc places screens left to right in the order their connectors are
 * enumerated, which has nothing to do with how the monitors are physically
 * arranged. This is how a compositor overrides that -- from a config file, or
 * on behalf of a client speaking an output-configuration protocol.
 *
 * Nothing validates the result: screens may be left overlapping or with gaps
 * between them, exactly as the caller asked. Only the logical position changes;
 * the mode and the scanout are untouched.
 *
 * Both wl_output.geometry and the screen's geometry_changed handler fire.
 * zxdg_output_v1.logical_position is *not* re-sent -- swc does not keep those
 * resources -- so a client that bound xdg-output before the move keeps a stale
 * logical position until it rebinds.
 */
void
swc_screen_set_position(struct swc_screen *screen, int32_t x, int32_t y);

/**
 * Get the registry name of the wl_output global corresponding to this screen,
 * as seen by the given client.
 *
 * This lets a compositor tell a client which wl_output one of its own protocol
 * objects refers to. Names are per-client, so the client must be the one that
 * will receive the name.
 *
 * A screen may drive several outputs, in which case this reports the first.
 * Returns false if the screen has no output, or none the client can see.
 */
bool
swc_screen_get_wl_output_name(struct swc_screen *screen,
                              struct wl_client *client, uint32_t *name);

/**
 * Get the registry name of the compositor's wl_seat global, as seen by the
 * given client.
 *
 * The counterpart of swc_screen_get_wl_output_name for input. swc has a single
 * seat, so there is no seat argument.
 *
 * Returns false if the client has not been shown the global.
 */
bool
swc_get_wl_seat_name(struct wl_client *client, uint32_t *name);

/* }}} */

/**
 * Stacking layers, ordered from back to front.
 *
 * Anything is always stacked within its layer: nothing in SWC_STACK_LAYER_TOP
 * can be drawn below something in SWC_STACK_LAYER_NORMAL, whatever the
 * per-item ordering says. New windows start in SWC_STACK_LAYER_NORMAL.
 */
enum swc_stack_layer {
	SWC_STACK_LAYER_BACKGROUND = 0,
	SWC_STACK_LAYER_BOTTOM = 1,
	SWC_STACK_LAYER_NORMAL = 2,
	SWC_STACK_LAYER_TOP = 3,
	SWC_STACK_LAYER_OVERLAY = 4,
};

/* Shell surfaces {{{ */

/**
 * A surface the compositor displays and positions directly, with no shell
 * protocol involved.
 *
 * Windows come from xdg-shell and negotiate their size; a shell surface does
 * not. It is for a compositor's own interface -- bars, overlays, menus drawn
 * by a window manager rather than by an application. The compositor decides
 * where it goes and the client just draws.
 */
struct swc_shell_surface;

/**
 * Start displaying a wl_surface as a shell surface.
 *
 * The surface must be a wl_surface that has not already been given a role.
 * Returns NULL if it is not, or on allocation failure.
 *
 * The result is not shown until swc_shell_surface_show.
 */
struct swc_shell_surface *
swc_shell_surface_create(struct wl_resource *surface);

/**
 * Stop displaying the surface. The wl_surface itself is untouched.
 */
void
swc_shell_surface_destroy(struct swc_shell_surface *shell_surface);

void
swc_shell_surface_show(struct swc_shell_surface *shell_surface);
void
swc_shell_surface_hide(struct swc_shell_surface *shell_surface);

/**
 * Position the surface in compositor-global coordinates.
 */
void
swc_shell_surface_set_position(struct swc_shell_surface *shell_surface,
                               int32_t x, int32_t y);

/**
 * Stacking, matching the window equivalents. A compositor's own interface
 * usually belongs in SWC_STACK_LAYER_OVERLAY or _TOP.
 */
void
swc_shell_surface_set_stack_layer(struct swc_shell_surface *shell_surface,
                                  enum swc_stack_layer layer);
void
swc_shell_surface_raise(struct swc_shell_surface *shell_surface);
void
swc_shell_surface_lower(struct swc_shell_surface *shell_surface);

/* }}} */

/* Windows {{{ */

/**
 * Who draws a window's decorations.
 *
 * Values match xdg-decoration-unstable-v1, except for
 * SWC_DECORATION_MODE_NONE, which that protocol expresses as the absence of a
 * preference rather than as a mode.
 */
enum swc_decoration_mode {
	SWC_DECORATION_MODE_NONE = 0,
	SWC_DECORATION_MODE_CLIENT_SIDE = 1,
	SWC_DECORATION_MODE_SERVER_SIDE = 2,
};

struct swc_window_handler {
	/**
	 * Called when the window is about to be destroyed.
	 *
	 * After this is called, the window is no longer valid.
	 */
	void (*destroy)(void *data);

	/**
	 * Called when the window's title changes.
	 */
	void (*title_changed)(void *data);

	/**
	 * Called when the window's application identifier changes.
	 */
	void (*app_id_changed)(void *data);

	/**
	 * Called when the window's parent changes.
	 *
	 * This can occur when the window becomes a transient for another window, or
	 * becomes a toplevel window.
	 */
	void (*parent_changed)(void *data);

	/**
	 * Called when the pointer enters the window.
	 */
	void (*entered)(void *data);

	/**
	 * Called when the pointer leaves the window.
	 *
	 * The counterpart of entered. A window manager tracking which window the
	 * pointer is over needs both; with only entered it can tell where the
	 * pointer went but never that it left everything.
	 */
	void (*left)(void *data);

	/**
	 * Called when the window wants to initiate an interactive move, but the
	 * window is not in stacked mode.
	 *
	 * The window manager may respond by changing the window's mode, after which
	 * the interactive move will be honored.
	 */
	void (*move)(void *data);

	/**
	 * Called when the window wants to initiate an interactive resize, but the
	 * window is not in stacked mode.
	 *
	 * The window manager may respond by changing the window's mode, after which
	 * the interactive resize will be honored.
	 */
	void (*resize)(void *data);

	/**
	 * Called when the window asks to be maximized.
	 *
	 * This is a request, not a notification: nothing has changed when it
	 * arrives. The window manager decides whether to honor it, typically by
	 * calling swc_window_set_geometry with the screen's usable geometry.
	 */
	void (*maximize)(void *data);

	/**
	 * Called when the window asks to stop being maximized.
	 *
	 * As with maximize, the window manager decides whether to honor it.
	 */
	void (*unmaximize)(void *data);

	/**
	 * Called when the window asks to be minimized.
	 *
	 * swc has no concept of minimized, so a window manager wanting to support
	 * it must implement it, for example with swc_window_hide.
	 */
	void (*minimize)(void *data);

	/**
	 * Called when the window asks for its window menu to be shown, at the
	 * given position relative to the window.
	 *
	 * There is no menu in swc; a window manager that does not draw one should
	 * leave this NULL.
	 */
	void (*window_menu)(void *data, int32_t x, int32_t y);

	/**
	 * Called when the window states a preference for who draws its
	 * decorations, or withdraws one with SWC_DECORATION_MODE_NONE.
	 *
	 * This is a preference, not a decision. Reply with
	 * swc_window_set_decoration_mode; the window uses whatever it is told,
	 * regardless of what it asked for. If this is left NULL, the window is
	 * told SWC_DECORATION_MODE_SERVER_SIDE, which is what swc did
	 * unconditionally before this callback existed.
	 */
	void (*decoration_mode)(void *data, enum swc_decoration_mode mode);
};

struct swc_window {
	char *title;
	char *app_id;

	struct swc_window *parent;
	uint32_t motion_throttle_ms;
	uint32_t min_width;
	uint32_t min_height;
	uint32_t max_width;
	uint32_t max_height;
};

/**
 * Set the handler associated with this window.
 */
void
swc_window_set_handler(struct swc_window *window,
                       const struct swc_window_handler *handler, void *data);

/**
 * Request that the specified window close.
 */
void
swc_window_close(struct swc_window *window);

/**
 * Make the specified window visible.
 */
void
swc_window_show(struct swc_window *window);

/**
 * Make the specified window hidden.
 */
void
swc_window_hide(struct swc_window *window);

/**
 * Set the keyboard focus to the specified window.
 *
 * If window is NULL, the keyboard will have no focus.
 */
void
swc_window_focus(struct swc_window *window);

/**
 * Sets the window to stacked mode.
 *
 * A window in this mode has its size specified by the client. The window's
 * viewport will be adjusted to the size of the buffer attached by the
 * client.
 *
 * Use of this mode is required to allow interactive moving and resizing.
 */
void
swc_window_set_stacked(struct swc_window *window);

/**
 * Sets the window to tiled mode.
 *
 * A window in this mode has its size specified by the window manager.
 * Additionally, swc will configure the window to operate in a tiled or
 * maximized state in order to prevent the window from drawing shadows.
 *
 * It is invalid to interactively move or resize a window in tiled mode.
 */
void
swc_window_set_tiled(struct swc_window *window);

/**
 * Sets the window to fullscreen mode.
 */
void
swc_window_set_fullscreen(struct swc_window *window, struct swc_screen *screen);

/**
 * Set the window's position.
 *
 * The x and y coordinates refer to the top-left corner of the actual contents
 * of the window and should be adjusted for the border size.
 */
void
swc_window_set_position(struct swc_window *window, int32_t x, int32_t y);

/**
 * Set the window's size.
 *
 * The width and height refer to the dimension of the actual contents of the
 * window and should be adjusted for the border size.
 */
void
swc_window_set_size(struct swc_window *window, uint32_t width, uint32_t height);

/**
 * Set the window's size and position.
 *
 * This is a convenience function that is equivalent to calling
 * swc_window_set_size and then swc_window_set_position.
 */
void
swc_window_set_geometry(struct swc_window *window,
                        const struct swc_rectangle *geometry);

/**
 * Begin a transaction.
 *
 * Normally each window's new geometry is applied as soon as that window
 * responds, so a relayout touching several windows lands in pieces across
 * several frames and the user sees it tear.
 *
 * Inside a transaction, every window reconfigured with swc_window_set_size,
 * swc_window_set_position or swc_window_set_geometry is instead collected into
 * a cohort. Nothing is applied, and the windows' own commits are held back from
 * the screen, until every member has responded. The whole relayout then appears
 * in a single frame.
 *
 * Returns false if a transaction is already open, or on allocation failure.
 * Transactions do not nest.
 */
bool
swc_transaction_begin(void);

/**
 * Finish a transaction and wait for the windows to respond.
 *
 * The cohort completes when every window has acknowledged, or after
 * timeout_ms milliseconds, whichever comes first. `done` is then called with
 * timed_out indicating which happened, and may be NULL.
 *
 * Completing does not display anything. The cohort is held until
 * swc_transaction_present is called, which leaves room to inspect the
 * resulting geometry and make further changes before any of it is shown.
 *
 * A window that did not respond in time keeps its pending state and falls back
 * to being applied on its own when its next buffer arrives, so a slow or wedged
 * client delays the relayout but never blocks it.
 *
 * If nothing was reconfigured, the transaction completes immediately and `done`
 * runs before this function returns.
 */
void
swc_transaction_commit(uint32_t timeout_ms,
                       void (*done)(bool timed_out, void *data), void *data);

/**
 * Display the results of a completed transaction.
 *
 * Applies the new geometry and releases the windows' held content, so the whole
 * relayout reaches the screen in one frame. Call this once the transaction's
 * completion callback has run; calling it earlier, or with no transaction
 * outstanding, does nothing.
 *
 * swc_transaction_begin presents any outstanding cohort first, so forgetting to
 * call this delays a relayout but never leaves windows frozen.
 */
void
swc_transaction_present(void);

/**
 * Whether a transaction is currently open.
 */
bool
swc_transaction_active(void);

/**
 * Get the size the window has committed, which is not necessarily the size it
 * is currently displaying.
 *
 * swc_window_get_geometry reports what is on screen. Inside a transaction that
 * is deliberately stale: the window has acknowledged its new size and
 * committed a buffer, but the content is held back until
 * swc_transaction_present. This reports the size that commit carries, so a
 * window manager can act on the dimensions a window actually took without
 * waiting for them to be displayed.
 *
 * Returns false if the window has not committed a buffer yet.
 */
bool
swc_window_get_committed_size(struct swc_window *window, uint32_t *width,
                              uint32_t *height);

/**
 * Get the window's current geometry in compositor-global coordinates.
 */
bool
swc_window_get_geometry(const struct swc_window *window,
                        struct swc_rectangle *geometry);

/**
 * Clip the window, including its borders and decorations, to a box relative
 * to the window's content geometry. A zero width or height disables clipping.
 */
void
swc_window_set_clip_box(struct swc_window *window, int32_t x, int32_t y,
                        uint32_t width, uint32_t height);

/**
 * Clip only the window content to a box relative to the window's content
 * geometry. A zero width or height disables content clipping.
 */
void
swc_window_set_content_clip_box(struct swc_window *window, int32_t x,
                                int32_t y, uint32_t width, uint32_t height);

/**
 * Get the pid of the client that owns this window
 *
 * returns pid, or 0 if unavailable
 */
pid_t
swc_window_get_pid(struct swc_window *window);

/**
 * Set the window's border color and width.
 *
 * NOTE: The window's geometry remains unchanged, and should be updated if a
 *       fixed top-left corner of the border is desired.
 *
 * info from dalem: unsure how much double borders break!
 */
void
swc_window_set_border(struct swc_window *window, uint32_t inner_border_color,
                      uint32_t inner_border_width, uint32_t outer_border_color,
                      uint32_t outer_border_width);

/*window decor things*/

/**
 * Select which decor edge a text slot is rendered on.
 */
enum swc_decor_edge {
	SWC_DECOR_EDGE_TOP,
	SWC_DECOR_EDGE_RIGHT,
	SWC_DECOR_EDGE_BOTTOM,
	SWC_DECOR_EDGE_LEFT,
};

/**
 * Choose text alignment within the selected decor edge.
 *
 * for top and bottom edges, alignment is horizontal
 * for left and right edges, alignment is vertical
 */
enum swc_decor_align {
	SWC_DECOR_ALIGN_START,
	SWC_DECOR_ALIGN_CENTER,
	SWC_DECOR_ALIGN_END,
};

/**
 * Describe a window decor text slot.
 *
 * if enabled is false, no text is drawn.
 *
 * the wm supplies some string to be rendered in this decor slot. swc
 * copies the string when swc_window_set_decor() is called, so caller doesn't
 * need to keep it after the call returns.
 *
 * for top and bottom edges, text is drawn horizontally
 * for left and right edges, text is drawn as stacked glyphs, one glyph
 * per row.
 *
 * the font field accepts some fontconfig pattern such as "sans-serif:size=10".
 * if font is NULL, a default font is used.
 */
struct swc_decor_text {
	bool enabled;
	enum swc_decor_edge edge;
	enum swc_decor_align align;
	const char *string;
	uint32_t color;
	uint32_t padding;
	int32_t offset_x, offset_y;
	const char *font;
};

/**
 * Describes one wm-supplied decor pixel block.
 *
 * swc copies the pixel data when swc_window_set_decor() is called, so caller
 * doesn't need to keep it after the call returns.
 *
 * Pixel data is expected to be ARGB8888 with the provided stride.
 */
struct swc_decor_part {
	uint32_t width, height;
	uint32_t stride;
	const void *data;
};

/**
 * Describes optional pixel blocks for the outer frame of a decor.
 *
 * corner parts are drawn once at their matching corner
 * edge parts are tiled to fill the remaining edge area between corners.
 */
struct swc_decor_parts {
	struct swc_decor_part top_left;
	struct swc_decor_part top;
	struct swc_decor_part top_right;
	struct swc_decor_part left;
	struct swc_decor_part right;
	struct swc_decor_part bottom_left;
	struct swc_decor_part bottom;
	struct swc_decor_part bottom_right;
};

/**
 * Describe decor around a window's edges.
 *
 * The edge sizes extend outward from the window content geometry.
 * if you put 0 it won't be visible.
 *
 * The title field controls an optional text slot rendered on one edge.
 */
struct swc_decor {
	uint32_t color;
	uint32_t top, right, bottom, left;
	const struct swc_decor_parts *parts;
	struct swc_decor_text title;
};

/**
 * Set window decor around the window.
 *
 * the dimensions are independent for each edge and extend outward from the
 * window content geometry.
 *
 * swc copies any decor text string needed by the configuration.
 *
 * passing NULL disables window decor.
 */
void
swc_window_set_decor(struct swc_window *window, const struct swc_decor *decor);

/**
 * Begin an interactive move of the specified window.
 */
void
swc_window_begin_move(struct swc_window *window);

/**
 * End an interactive move of the specified window.
 */
void
swc_window_end_move(struct swc_window *window);

enum {
	SWC_WINDOW_EDGE_AUTO = 0,
	SWC_WINDOW_EDGE_TOP = (1 << 0),
	SWC_WINDOW_EDGE_BOTTOM = (1 << 1),
	SWC_WINDOW_EDGE_LEFT = (1 << 2),
	SWC_WINDOW_EDGE_RIGHT = (1 << 3)
};

/**
 * Begin an interactive resize of the specified window.
 */
void
swc_window_begin_resize(struct swc_window *window, uint32_t edges);

/**
 * End an interactive resize of the specified window.
 */
void
swc_window_end_resize(struct swc_window *window);

/**
 * returns the topmost window at any given compositor global coordinates
 *
 * returns null if there is no window at that point
 */
struct swc_window *
swc_window_at(int32_t x, int32_t y);

/**
 * move a window in the stacking order by one step
 *
 * direction < 0 moves the window towards the front (higher)
 * direction > 0 moves the window towards the back (lower)
 */
void
swc_window_stack(struct swc_window *window, int32_t direction);

/**
 * Stacking layers, ordered from back to front.
 *
 * A window is always stacked within its layer: nothing in SWC_STACK_LAYER_TOP
 * can be drawn below something in SWC_STACK_LAYER_NORMAL, whatever the
 * per-window ordering says. New windows start in SWC_STACK_LAYER_NORMAL.
 */
/**
 * Move the window to a stacking layer, placing it at the front of that layer.
 *
 * Use swc_window_raise or swc_window_lower to reposition it within the layer
 * afterwards.
 */
void
swc_window_set_stack_layer(struct swc_window *window,
                           enum swc_stack_layer layer);

/**
 * Move the window to the front of its stacking layer.
 */
void
swc_window_raise(struct swc_window *window);

/**
 * Move the window to the back of its stacking layer.
 */
void
swc_window_lower(struct swc_window *window);

/**
 * Stack the window directly above or below another window.
 *
 * Has no effect if either window is invalid, or if they are the same window.
 * The two windows do not have to be in the same stacking layer, but the layer
 * ordering still wins: restacking a window relative to a sibling in another
 * layer will not lift it out of its own.
 */
void
swc_window_restack(struct swc_window *window, struct swc_window *sibling,
                   bool above);

/**
 * Tell the window who draws its decorations.
 *
 * Has no effect if the window never asked, which is the case for clients that
 * do not bind xdg-decoration. SWC_DECORATION_MODE_NONE is not a valid answer
 * and is ignored.
 */
void
swc_window_set_decoration_mode(struct swc_window *window,
                               enum swc_decoration_mode mode);

/* }}} */

/* Keyboard repeat rate (characters per second) and delay (ms) definitions.
 * Exposed as extern so compositors can set them. 
 */
extern int32_t swc_repeat_rate, swc_repeat_delay;

/* Bindings {{{ */

enum {
	SWC_MOD_CTRL = 1 << 0,
	SWC_MOD_ALT = 1 << 1,
	SWC_MOD_LOGO = 1 << 2,
	SWC_MOD_SHIFT = 1 << 3,
	/*
	 * xkb's Mod3 and Mod5. Alt and Logo above are xkb's Mod1 and Mod4, so
	 * these complete the set a keymap can actually produce; without them a
	 * binding on either is silently unmatchable.
	 */
	SWC_MOD_MOD3 = 1 << 4,
	SWC_MOD_MOD5 = 1 << 5,
	SWC_MOD_ANY = ~0
};

enum swc_binding_type {
	SWC_BINDING_KEY,
	SWC_BINDING_BUTTON,
};

typedef void (*swc_binding_handler)(void *data, uint32_t time, uint32_t value,
                                    uint32_t state);
typedef void (*swc_axis_binding_handler)(void *data, uint32_t time,
                                         uint32_t axis, int32_t value120);

/**
 * Register a new input binding.
 *
 * Returns 0 on success, negative error code otherwise.
 */
int
swc_add_binding(enum swc_binding_type type, uint32_t modifiers, uint32_t value,
                swc_binding_handler handler, void *data);

/**
 * Unregister a registered input binding.
 *
 */
void
swc_remove_binding(enum swc_binding_type type, uint32_t modifiers,
                   uint32_t value);

/**
 * Observe raw keyboard events.
 *
 * swc_add_binding() answers "call me when this exact combination is pressed",
 * which is all most compositors want. A compositor implementing a window
 * management protocol needs more: to resolve a key in a layout other than the
 * active one, to know which modifiers a keysym consumed, to swallow a key that
 * matches nothing, and to see presses that no binding claimed. All of that
 * needs the raw event.
 *
 * Observers run *after* swc's own bindings and before the event is delivered to
 * the focused client. That order is deliberate: VT switching and the terminate
 * binding are registered internally with swc_add_binding(), and an observer
 * that could swallow them would be able to trap the user on a wedged VT.
 *
 * Returning true from `key` consumes the event -- it does not reach the client.
 * Both modifier masks are SWC_MOD_* values.
 */
struct swc_keyboard_observer {
	bool (*key)(void *data, uint32_t time, uint32_t keycode, uint32_t state);
	void (*modifiers)(void *data, uint32_t previous, uint32_t current);
};

int
swc_add_keyboard_observer(const struct swc_keyboard_observer *observer,
                          void *data);
void
swc_remove_keyboard_observer(const struct swc_keyboard_observer *observer,
                             void *data);

/**
 * Keyboard state, for compositors doing their own key matching.
 *
 * Deliberately narrow: these hand back plain integers rather than exposing
 * xkbcommon types, so swc.h imposes no dependency on its consumers. The cost is
 * that each new need means a new accessor here -- see the note in swc_keyboard
 * accessors' implementation for what that trade is expected to cost.
 *
 * Keycodes are the values swc uses everywhere else, i.e. Linux evdev codes; the
 * xkb offset is applied internally.
 */
uint32_t
swc_keyboard_keysym(uint32_t keycode);

/**
 * The keysym a keycode produces in a specific layout, ignoring which layout is
 * active. `layout` is a 0-indexed xkb layout number. Returns 0 if the layout
 * does not exist or the key produces nothing in it.
 */
uint32_t
swc_keyboard_keysym_in_layout(uint32_t keycode, uint32_t layout);

/**
 * The modifiers currently held, as an SWC_MOD_* mask.
 */
uint32_t
swc_keyboard_modifiers(void);

/**
 * The modifiers consumed producing the current keysym for this keycode, as an
 * SWC_MOD_* mask.
 *
 * A binding should compare against the active modifiers minus these, or a
 * keysym that requires a modifier can never be matched: pressing shift+2 for
 * "@" leaves shift active, and a binding on plain "@" would otherwise miss.
 */
uint32_t
swc_keyboard_consumed_modifiers(uint32_t keycode);

/**
 * Number of layouts in the active keymap, for validating a layout override.
 */
uint32_t
swc_keyboard_num_layouts(void);

/**
 * The layout currently in effect for this keycode, 0-indexed.
 *
 * Needed to ask swc_keyboard_keysym_in_layout() for the *unshifted* symbol of
 * the active layout, which is what a binding names.
 */
uint32_t
swc_keyboard_layout(uint32_t keycode);

/**
 * register a new pointer axis binding
 *
 * this will intercept axis events from clients; use swc_pointer_send_axis()
 * from the handler to forward events when appropriate
 */
int
swc_add_axis_binding(uint32_t modifiers, uint32_t axis,
                     swc_axis_binding_handler handler, void *data);

/* }}} */

/**
 * This is a user-provided structure that swc will use to notify the display
 * server of new windows, screens and input devices.
 */
struct swc_manager {
	/**
	 * Called when a new screen is created.
	 */
	void (*new_screen)(struct swc_screen *screen);

	/**
	 * Called when a new window is created.
	 */
	void (*new_window)(struct swc_window *window);

	/**
	 * Called when a new input device is detected.
	 */
	void (*new_device)(struct libinput_device *device);

	/**
	 * Called when the session gets activated (for example, startup or VT
	 * switch).
	 */
	void (*activate)(void);

	/**
	 * Called when the session gets deactivated.
	 */
	void (*deactivate)(void);
};

/**
 * Initializes the compositor using the specified display, event_loop, and
 * manager.
 */
bool
swc_initialize(struct wl_display *display, struct wl_event_loop *event_loop,
               const struct swc_manager *manager);

/**
 * Stops the compositor, releasing any used resources.
 */
void
swc_finalize(void);

#ifdef __cplusplus
}
#endif

#endif

/* vim: set fdm=marker : */
