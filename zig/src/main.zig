// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 neoswc contributors
//
// A minimal window manager in Zig, to prove libswc is usable from Zig.
//
// It does what example/wm.c does in miniature: tiles windows in a column,
// relayouts inside a transaction so the whole column moves in one frame, and
// answers the decoration and state requests. Deliberately small -- the point
// is that the C API binds, not that this is a good window manager.

const std = @import("std");
const c = @import("c");

const max_windows = 16;

var windows: [max_windows]?*c.struct_swc_window = @splat(null);
var num_windows: usize = 0;
var active_screen: ?*c.struct_swc_screen = null;
var display: ?*c.struct_wl_display = null;

const border_width: u32 = 1;
const border_color_active: u32 = 0xff333388;
const border_color_normal: u32 = 0xff888888;
const arrange_timeout_ms: u32 = 100;

fn arrangeDone(timed_out: bool, data: ?*anyopaque) callconv(.c) void {
    _ = data;
    std.debug.print("zig-wm: relayout of {d} window(s) {s}\n", .{
        num_windows,
        if (timed_out) "timed out" else "complete",
    });
    c.swc_transaction_present();
}

fn arrange() void {
    const screen = active_screen orelse return;
    if (num_windows == 0) return;

    const usable = screen.*.usable_geometry;
    const rows: u32 = @intCast(num_windows);
    const height = usable.height / rows;

    _ = c.swc_transaction_begin();

    for (windows[0..num_windows], 0..) |maybe_window, i| {
        const window = maybe_window orelse continue;
        const row: u32 = @intCast(i);
        var geometry: c.struct_swc_rectangle = .{
            .x = usable.x + @as(i32, @intCast(border_width)),
            .y = usable.y + @as(i32, @intCast(row * height + border_width)),
            .width = usable.width - 2 * border_width,
            .height = height - 2 * border_width,
        };
        c.swc_window_set_geometry(window, &geometry);
    }

    c.swc_transaction_commit(arrange_timeout_ms, arrangeDone, null);
}

fn focus(window: ?*c.struct_swc_window) void {
    for (windows[0..num_windows]) |maybe_other| {
        const other = maybe_other orelse continue;
        c.swc_window_set_border(other, border_color_normal, border_width, 0, 0);
    }
    if (window) |w| {
        c.swc_window_set_border(w, border_color_active, border_width, 0, 0);
    }
    c.swc_window_focus(window);
}

fn windowDestroy(data: ?*anyopaque) callconv(.c) void {
    const window: *c.struct_swc_window = @ptrCast(@alignCast(data orelse return));

    var i: usize = 0;
    while (i < num_windows) : (i += 1) {
        if (windows[i] != window) continue;
        // Close the gap rather than leaving a hole; order is the layout.
        var j = i;
        while (j + 1 < num_windows) : (j += 1) windows[j] = windows[j + 1];
        num_windows -= 1;
        windows[num_windows] = null;
        break;
    }

    arrange();
    focus(if (num_windows > 0) windows[0] else null);
}

fn windowEntered(data: ?*anyopaque) callconv(.c) void {
    const window: *c.struct_swc_window = @ptrCast(@alignCast(data orelse return));
    focus(window);
}

// translate-c renders C enums as c_uint, so the parameter is not a Zig enum
// and cannot be @intFromEnum'd.
fn windowDecorationMode(data: ?*anyopaque, mode: c.enum_swc_decoration_mode) callconv(.c) void {
    const window: *c.struct_swc_window = @ptrCast(@alignCast(data orelse return));
    std.debug.print("zig-wm: decoration preference {d}\n", .{mode});
    // This manager draws borders itself, so it always answers server-side.
    c.swc_window_set_decoration_mode(window, c.SWC_DECORATION_MODE_SERVER_SIDE);
}

fn windowMaximize(data: ?*anyopaque) callconv(.c) void {
    _ = data;
    std.debug.print("zig-wm: maximize requested\n", .{});
}

// extern structs have no default field values, so every callback must be
// listed even when unused.
var window_handler: c.struct_swc_window_handler = .{
    .destroy = windowDestroy,
    .title_changed = null,
    .app_id_changed = null,
    .parent_changed = null,
    .entered = windowEntered,
    .move = null,
    .resize = null,
    .maximize = windowMaximize,
    .unmaximize = null,
    .minimize = null,
    .window_menu = null,
    .decoration_mode = windowDecorationMode,
};

fn newScreen(screen: ?*c.struct_swc_screen) callconv(.c) void {
    active_screen = screen;
    std.debug.print("zig-wm: screen added\n", .{});
}

fn newWindow(window: ?*c.struct_swc_window) callconv(.c) void {
    const w = window orelse return;
    if (num_windows == max_windows) return;

    windows[num_windows] = w;
    num_windows += 1;

    c.swc_window_set_handler(w, &window_handler, w);
    c.swc_window_set_tiled(w);
    c.swc_window_show(w);
    // Exercises the Tier 1 stacking API; a column never overlaps, so this is
    // only here to prove the symbol binds and is callable.
    c.swc_window_raise(w);
    arrange();
    focus(w);
}

var manager: c.struct_swc_manager = .{
    .new_screen = newScreen,
    .new_window = newWindow,
    .new_device = null,
    .activate = null,
    .deactivate = null,
};

pub fn main() !u8 {
    display = c.wl_display_create() orelse {
        std.debug.print("zig-wm: failed to create display\n", .{});
        return 1;
    };

    const socket = c.wl_display_add_socket_auto(display) orelse {
        std.debug.print("zig-wm: failed to add socket\n", .{});
        return 1;
    };
    _ = c.setenv("WAYLAND_DISPLAY", socket, 1);
    std.debug.print("zig-wm: listening on {s}\n", .{socket});

    if (!c.swc_initialize(display, null, &manager)) {
        std.debug.print("zig-wm: swc_initialize failed\n", .{});
        return 1;
    }

    c.wl_display_run(display);
    c.wl_display_destroy(display);
    return 0;
}
