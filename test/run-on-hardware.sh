#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 neoswc contributors
#
# Run neoswc on real hardware, from a spare VT.
#
# This has only ever run in the QEMU test VM. On real hardware it takes over a
# VT and DRM master, so run it from a VT that is not your session's.
#
#   1. Ctrl+Alt+F3   (any free VT; your session is on tty1)
#   2. log in
#   3. sudo /path/to/this/script [wm|river]
#   4. Ctrl+Alt+F1 to get back to your session, whatever happens
#
# Everything is logged to /tmp/neoswc-hw.log, which survives a wedged VT --
# read it from your normal session afterwards.
#
# Argument:
#   wm     (default) the self-contained example window manager. Try this
#          first: it needs no protocol client, so a failure is swc's DRM or
#          input path rather than anything protocol-related.
#   river  neoswc serving the river protocols, with rill as the manager.
#          Override the manager with RIVER_WM=/path/to/wm.
set -u

NEOSWC="${NEOSWC:-/nix/store/jr0xdn9mda055bl2p742fz4xlhnp0i4f-neoswc-0.0}"
MODE="${1:-wm}"
LOG=/tmp/neoswc-hw.log

if [ "$(id -u)" != 0 ]; then
	echo "run as root: swc-launch opens DRM devices and manages the VT." >&2
	echo "(installing it setuid would avoid this; see CLAUDE.md)" >&2
	exit 1
fi

if [ -n "${WAYLAND_DISPLAY:-}" ] || [ -n "${DISPLAY:-}" ]; then
	echo "refusing to run from inside a graphical session." >&2
	echo "switch to a free VT with Ctrl+Alt+F3 and run it there." >&2
	exit 1
fi

# wld's intel driver claims every device with vendor id 0x8086 -- there is no
# device_id check -- so on anything newer than its libdrm_intel path supports
# it matches, fails to create a renderer, and swc exits with
# "Could not create WLD DRM renderer". Seen on an Arc A770 (8086:56a0).
#
# WLD_DRM_DUMB skips driver probing and uses the dumb-buffer path with pixman
# software rendering, which is what the VM used all along. Set WLD_DRM_DUMB=
# (empty) to try the accelerated path instead.
export WLD_DRM_DUMB="${WLD_DRM_DUMB-1}"

: > "$LOG"
{
	echo "WLD_DRM_DUMB=${WLD_DRM_DUMB:-<unset, probing drivers>}"
	echo "gpu: $(lspci -nn 2>/dev/null | grep -iE 'VGA|Display' | head -1)"
	echo "=== $(date -Is) mode=$MODE ==="
	echo "dri: $(ls /dev/dri 2>&1)"
	echo "drm driver: $(readlink -f /sys/class/drm/card*/device/driver 2>/dev/null | sed 's|.*/||' | sort -u | tr '\n' ' ')"
	echo "input: $(ls /dev/input 2>/dev/null | tr '\n' ' ')"
	echo "vt: ${XDG_VTNR:-unknown}"
} >> "$LOG" 2>&1

# Cores here too, for the same reason the VM collects them: a stripped
# backtrace is worse than none.
ulimit -c unlimited 2>/dev/null || true
echo '/tmp/neoswc-hw-core.%e.%p' > /proc/sys/kernel/core_pattern 2>/dev/null || true

# Spawn one client once the compositor is up, so there is something on screen
# without needing a keybinding to work first. swc-launch sets XDG_RUNTIME_DIR
# to this when it is unset, and the socket lands there.
RT=/tmp/XDG_RUNTIME_DIR_0
(
	for _ in $(seq 1 40); do
		sock=$(ls "$RT"/wayland-* 2>/dev/null | grep -v '\.lock$' | head -1)
		if [ -n "$sock" ]; then
			XDG_RUNTIME_DIR="$RT" WAYLAND_DISPLAY="$(basename "$sock")" \
				"${TERMINAL:-foot}" >> "$LOG" 2>&1 &
			echo "spawned ${TERMINAL:-foot} on $(basename "$sock")" >> "$LOG"
			exit 0
		fi
		sleep 0.25
	done
	echo "no wayland socket appeared in $RT" >> "$LOG"
) &

case "$MODE" in
river)
	# rill is a client of neoswc, not a compositor, so it cannot be the
	# argument to swc-launch. neoswc spawns it once its socket is up.
	echo "starting neoswc (river protocols) with ${RIVER_WM:-rill}" >> "$LOG"
	"$NEOSWC/bin/swc-launch" -- "$NEOSWC/bin/neoswc" "${RIVER_WM:-rill}" >> "$LOG" 2>&1
	;;
wm)
	echo "starting the example window manager" >> "$LOG"
	"$NEOSWC/bin/swc-launch" -- "$NEOSWC/bin/neoswc-example-wm" >> "$LOG" 2>&1
	;;
*)
	echo "unknown mode: $MODE (want 'wm' or 'river')" >&2
	exit 1
	;;
esac

echo "exited with status $?" >> "$LOG"
echo
echo "finished; log is at $LOG"
