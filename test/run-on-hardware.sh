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
#   3. /path/to/this/script [wm|river]
#   4. Ctrl+Alt+F1 to get back to your session, whatever happens
#
# No sudo when the setuid swc-launch wrapper is installed, which is the point of
# installing it: the compositor and every client it spawns run as you.
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

MODE="${1:-wm}"
# Per-uid, because an earlier run as root leaves /tmp/neoswc-hw.log owned by
# root and every later run as your own user dies on it -- which reads as "the
# script is broken" rather than as a stale file.
LOG="/tmp/neoswc-hw-$(id -u).log"
if [ -e "$LOG" ] && [ ! -w "$LOG" ]; then
	echo "$LOG exists and is not writable by you (stale root-owned run?)." >&2
	echo "remove it: sudo rm -f $LOG" >&2
	exit 1
fi
REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

# Build from the working tree. A pinned store path used to live here, which
# meant the script silently tested whatever had been built when the path was
# written -- the one thing a "run it on real hardware" script must never do.
# Set NEOSWC to skip the build and use a specific store path.
if [ -z "${NEOSWC:-}" ]; then
	echo "building $REPO ..." >&2
	NEOSWC=$(nix build --no-link --print-out-paths "$REPO#neoswc") || exit 1
fi
echo "using $NEOSWC" >&2

# swc-launch needs DRM master and the VT. The setuid wrapper is the good path:
# launch.c spawns the compositor with POSIX_SPAWN_RESETIDS, so only the
# launcher keeps euid 0 and everything else runs as you. It comes from the
# installed package rather than the build above, which is fine as long as the
# launch protocol between swc-launch and libswc has not changed.
LAUNCH=/run/wrappers/bin/swc-launch
if [ ! -u "$LAUNCH" ]; then
	LAUNCH="$NEOSWC/bin/swc-launch"
	if [ "$(id -u)" != 0 ]; then
		echo "no setuid swc-launch wrapper, so this needs root." >&2
		echo "either run it with sudo, or install the wrapper; see CLAUDE.md." >&2
		exit 1
	fi
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
	echo "neoswc: $NEOSWC"
	echo "launcher: $LAUNCH"
	# seat.c calls keyboard_create(NULL), so libxkbcommon picks the defaults
	# from these and falls back to US. From a VT they are usually unset even
	# when the desktop session sets them, and every keysym-matched binding
	# misses on a non-US layout.
	echo "xkb: layout=${XKB_DEFAULT_LAYOUT:-<unset, libxkbcommon default>}" \
	     "variant=${XKB_DEFAULT_VARIANT:-<unset>}" \
	     "options=${XKB_DEFAULT_OPTIONS:-<unset>}"
} >> "$LOG" 2>&1

# Cores here too, for the same reason the VM collects them: a stripped
# backtrace is worse than none.
ulimit -c unlimited 2>/dev/null || true
echo '/tmp/neoswc-hw-core.%e.%p' > /proc/sys/kernel/core_pattern 2>/dev/null || true

# Spawn one client once the compositor is up, so there is something on screen
# without needing a keybinding to work first. swc-launch only invents an
# XDG_RUNTIME_DIR when there is none -- running as your own user from a VT there
# usually is one, and the socket lands there instead.
RT="${XDG_RUNTIME_DIR:-/tmp/XDG_RUNTIME_DIR_$(id -u)}"

# Which sockets already exist. Your own graphical session on another VT has one
# sitting in this same directory, and picking "the first wayland-* we find" hands
# every test client to *that* compositor instead of the one under test -- which
# looks like a working run right up until you read which socket it used. Wait
# for one that was not there before.
PRE_SOCKS=$(ls "$RT"/wayland-* 2>/dev/null | grep -v '\.lock$' | sort | tr '\n' ' ')
echo "sockets already present: ${PRE_SOCKS:-none}" >> "$LOG"
(
	for _ in $(seq 1 40); do
		sock=
		for s in $(ls "$RT"/wayland-* 2>/dev/null | grep -v '\.lock$' | sort); do
			case " $PRE_SOCKS " in
			*" $s "*) continue ;;
			esac
			sock=$s
			break
		done
		if [ -n "$sock" ]; then
			export XDG_RUNTIME_DIR="$RT"
			export WAYLAND_DISPLAY="$(basename "$sock")"

			# Monitor arrangement. The session's kanshi runs under
			# wm-session.target, which is gated on XDG_CURRENT_DESKTOP and so
			# never starts on a bare VT -- meaning the compositor offered
			# zwlr_output_management_v1 and nothing ever spoke it, and the
			# screens kept swc's connector-enumeration order. Start it here so
			# a hardware run tests the arrangement rather than assuming it.
			if command -v wlr-randr >/dev/null 2>&1; then
				echo "heads before kanshi:" >> "$LOG"
				wlr-randr >> "$LOG" 2>&1 || true
			fi
			if command -v kanshi >/dev/null 2>&1; then
				kanshi >> "$LOG" 2>&1 &
				echo "started kanshi" >> "$LOG"
				sleep 2
				if command -v wlr-randr >/dev/null 2>&1; then
					echo "heads after kanshi:" >> "$LOG"
					wlr-randr >> "$LOG" 2>&1 || true
				fi
			else
				echo "kanshi not found; screens keep swc's enumeration order" >> "$LOG"
			fi

			"${TERMINAL:-foot}" >> "$LOG" 2>&1 &
			echo "spawned ${TERMINAL:-foot} on $WAYLAND_DISPLAY" >> "$LOG"
			exit 0
		fi
		sleep 0.25
	done
	echo "no NEW wayland socket appeared in $RT (had: ${PRE_SOCKS:-none})" >> "$LOG"
) &

case "$MODE" in
river)
	# rill is a client of neoswc, not a compositor, so it cannot be the
	# argument to swc-launch. neoswc spawns it once its socket is up.
	echo "starting neoswc (river protocols) with ${RIVER_WM:-rill}" >> "$LOG"
	"$LAUNCH" -- "$NEOSWC/bin/neoswc" "${RIVER_WM:-rill}" >> "$LOG" 2>&1
	;;
wm)
	echo "starting the example window manager" >> "$LOG"
	"$LAUNCH" -- "$NEOSWC/bin/neoswc-example-wm" >> "$LOG" 2>&1
	;;
*)
	echo "unknown mode: $MODE (want 'wm' or 'river')" >&2
	exit 1
	;;
esac

echo "exited with status $?" >> "$LOG"
echo
echo "finished; log is at $LOG"
