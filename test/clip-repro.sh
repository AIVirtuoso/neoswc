#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 neoswc contributors
#
# Show that subsurfaces are not clipped.
#
# river's set_clip_box restricts a window to a box, but the box lives on the
# toplevel's compositor_view and a subsurface gets its own -- so a client that
# draws its own decorations keeps painting them at full size while its content
# clips. This runs the VM twice and leaves two screendumps to compare.
#
#   ./test/clip-repro.sh            both runs, into /tmp/neoswc-clip-repro
#   ./test/clip-repro.sh DIR        somewhere else
#
# Look at DIR/baseline.png and DIR/clipped.png. In the clipped one the terminal
# body is gone -- the background shows through -- and foot's title bar is still
# drawn across the full width of the screen. That title bar is the bug.
#
# Two knobs make it visible, and both are off by default:
#   /tmp/neoswc-vm-share/csd    force client-side decorations, so foot creates
#                               subsurfaces at all. With server-side ones it
#                               creates none and there is nothing to leak.
#   /tmp/neoswc-vm-share/clip   the existing clip test: first window to 1x1.
set -uo pipefail

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=${1:-/tmp/neoswc-clip-repro}
SHARE=/tmp/neoswc-vm-share
MON=/tmp/neoswc-vm-monitor.sock

# socat is not installed on a stock NixOS system and neither is it a build
# input, so fall back to fetching it. A missing tool here used to fail silently
# -- the run completed, the screendumps did not, and the result looked like the
# VM misbehaving rather than like the host lacking a program.
socat_cmd() {
	if command -v socat >/dev/null 2>&1; then
		socat "$@"
	else
		nix run nixpkgs#socat -- "$@"
	fi
}

for t in magick qemu-system-x86_64; do
	command -v "$t" >/dev/null 2>&1 || {
		echo "$t is not installed; this needs it to capture frames." >&2
		exit 1
	}
done

if [ ! -x "$REPO/result/bin/run-nixos-vm" ]; then
	echo "building the VM ..." >&2
	nix build "$REPO#vm" || exit 1
fi

run() {
	local name=$1 clip=$2

	pkill -f '[q]emu-system-x86_64' 2>/dev/null || true
	sleep 1
	mkdir -p "$SHARE"
	rm -f "$SHARE"/* 2>/dev/null || true

	echo river > "$SHARE/mode"
	touch "$SHARE/csd"      # client-side decorations => subsurfaces
	touch "$SHARE/nofuzzel" # irrelevant here, and it steals keyboard focus
	[ "$clip" = none ] || echo "$clip" > "$SHARE/clip"

	echo "== $name run (clip=$clip)" >&2
	( cd "$REPO" && QEMU_OPTS="-display none" ./result/bin/run-nixos-vm ) \
		> "$OUT/$name-console.log" 2>&1 &
	local vm=$! seen="" deadline=$((SECONDS + 300))

	while [ $SECONDS -lt $deadline ]; do
		kill -0 $vm 2>/dev/null || break
		for m in "$SHARE"/mark-*; do
			[ -e "$m" ] || continue
			local n
			n=$(basename "$m" | sed 's/^mark-//')
			case " $seen " in *" $n "*) continue ;; esac
			seen="$seen $n"
			# Only the first window is clipped, so the one-window frame is
			# the one worth keeping.
			if [ "$n" = one ]; then
				printf 'screendump %s\n' "$OUT/$name.ppm" \
					| socat_cmd - "UNIX-CONNECT:$MON" >/dev/null 2>&1
				sleep 0.8
				if [ -s "$OUT/$name.ppm" ]; then
					magick "$OUT/$name.ppm" "$OUT/$name.png" \
						&& rm -f "$OUT/$name.ppm"
					echo "   captured $OUT/$name.png" >&2
				else
					echo "   FAILED to capture a frame" >&2
				fi
			fi
			touch "$SHARE/go-$n"
		done
		[ -e "$SHARE/quit" ] && break
		sleep 0.2
	done

	touch "$SHARE/quit"
	wait $vm 2>/dev/null
	cp "$SHARE/smoke.log" "$OUT/$name-smoke.log" 2>/dev/null
}

rm -rf "$OUT"
mkdir -p "$OUT"
run baseline none
run clipped full

echo
if [ -s "$OUT/baseline.png" ] && [ -s "$OUT/clipped.png" ]; then
	echo "compare:"
	echo "  $OUT/baseline.png   window with its title bar, content underneath"
	echo "  $OUT/clipped.png    content clipped away, title bar still full width"
else
	echo "one or both frames are missing; see $OUT/*-console.log" >&2
	exit 1
fi
