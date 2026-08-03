// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 neoswc contributors

const std = @import("std");
const Translator = @import("translate_c").Translator;

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // libswc is built by meson, not installed, so point at the build tree.
    // Defaults to the in-tree meson build directory.
    const swc_build_dir = b.option(
        []const u8,
        "swc-build-dir",
        "meson build directory containing libswc (default: ../build)",
    ) orelse "../build";

    const translate_c: Translator = .init(b.dependency("translate_c", .{}), .{
        .name = "c",
        .c_source_file = b.path("c.h"),
        .target = target,
        .optimize = optimize,
    });
    // Translator's own methods, not translate_c.mod's: these update both the
    // module and the translate-c command line. The module's only do the former,
    // so translation silently misses them.
    translate_c.addIncludePath(b.path("../libswc"));
    translate_c.linkSystemLibrary("wayland-server", .{});
    addNixLibcIncludes(b, translate_c);

    const exe = b.addExecutable(.{
        .name = "neoswc-zig-wm",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        }),
    });
    exe.root_module.addImport("c", translate_c.mod);
    exe.root_module.addIncludePath(b.path("../libswc"));
    exe.root_module.addLibraryPath(.{ .cwd_relative = b.pathJoin(&.{ swc_build_dir, "libswc" }) });
    exe.root_module.linkSystemLibrary("swc", .{});
    exe.root_module.linkSystemLibrary("wayland-server", .{});

    b.installArtifact(exe);
}

/// On NixOS there is no /usr/include, and glibc's headers are not in
/// NIX_CFLAGS_COMPILE either -- the cc wrapper injects them separately. Aro's
/// native header detection therefore finds nothing and translation fails on
/// `#include <sys/types.h>`.
///
/// Nix records those paths in $NIX_CC/nix-support/libc-cflags, which is the
/// supported way to ask for them. No-op off NixOS, where Aro's own detection
/// works.
fn addNixLibcIncludes(b: *std.Build, translator: Translator) void {
    const nix_cc = b.graph.environ_map.get("NIX_CC") orelse return;
    const path = b.pathJoin(&.{ nix_cc, "nix-support", "libc-cflags" });
    // b.run rather than std.Io.Dir: reading one small file at configure time
    // is not worth tracking the 0.16 filesystem API for.
    const contents = b.run(&.{ "cat", path });

    // Entries look like `-idirafter /nix/store/...-glibc-dev/include`.
    var it = std.mem.tokenizeAny(u8, contents, " \n\t");
    while (it.next()) |token| {
        if (!std.mem.eql(u8, token, "-idirafter") and
            !std.mem.eql(u8, token, "-isystem")) continue;
        const dir = it.next() orelse break;
        translator.addSystemIncludePath(.{ .cwd_relative = b.dupe(dir) });
    }
}
