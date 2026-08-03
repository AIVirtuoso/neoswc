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
set -u

NEOSWC="${NEOSWC:-/nix/store/n9zmd3mh6lpjhxqac4pfdrkvyir6sgs5-neoswc-0.0}"
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

: > "$LOG"
{
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

case "$MODE" in
river)
	echo "starting neoswc (river protocols); rill must be started separately" >> "$LOG"
	"$NEOSWC/bin/swc-launch" -- "$NEOSWC/bin/neoswc" >> "$LOG" 2>&1
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
