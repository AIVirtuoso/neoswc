#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 neoswc contributors
#
# Demonstrate that subsurfaces escape river's set_clip_box.
#
# The clip box is stored on the toplevel's compositor_view, and a subsurface
# gets its own (subsurface.c:502), so a client that draws its own decorations
# keeps painting them at full size while its content clips.
#
# Three VM runs, because two are not enough to prove anything:
#
#   baseline      client-side decorations, no clip   -> title bar + content
#   clipped-csd   client-side decorations, clipped   -> title bar, no content
#   clipped-ssd   server-side decorations, clipped   -> nothing            <- control
#
# The control is what makes it evidence rather than an anecdote. Without it,
# "the title bar is still there" could just mean the title bar was never inside
# the box. clipped-ssd shows a clip box removing a whole window when that window
# has no subsurfaces, so the only thing differing between it and clipped-csd is
# whether the client created any.
#
#   ./test/clip-repro.sh [DIR]     default DIR is /tmp/neoswc-clip-repro
#
# Leaves the three frames, a stacked evidence.png, and prints a verdict.
# Exit status: 0 if the measurements were conclusive either way, 1 if the runs
# did not produce usable frames.
set -uo pipefail

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=${1:-/tmp/neoswc-clip-repro}
SHARE=/tmp/neoswc-vm-share
MON=/tmp/neoswc-vm-monitor.sock

# The window under test is the first one, which the test manager always places
# at the top of the screen. Sampled as bands rather than single pixels so the
# result does not depend on how many windows had appeared by capture time.
TITLE_ROW=13
BODY_TOP=120
BODY_HEIGHT=180
BG='#FF00FF' # swaybg, magenta: in nothing else on screen

# socat is not installed on a stock NixOS system and is not a build input.
# Falling back rather than failing, because the previous version of this piped a
# screendump through it with the output discarded -- so a missing socat produced
# a run that completed having captured nothing, which looked like the VM
# misbehaving rather than the host lacking a program.
socat_cmd() {
	if command -v socat >/dev/null 2>&1; then
		socat "$@"
	else
		nix run nixpkgs#socat -- "$@"
	fi
}

for t in magick qemu-system-x86_64; do
	command -v "$t" >/dev/null 2>&1 || {
		echo "$t is not installed; this needs it." >&2
		exit 1
	}
done

if [ ! -x "$REPO/result/bin/run-nixos-vm" ]; then
	echo "building the VM ..." >&2
	nix build "$REPO#vm" || exit 1
fi

# csd: force client-side decorations, so the client creates subsurfaces at all.
# clip: the existing clip test, which restricts the first window to 1x1.
run() {
	local name=$1 csd=$2 clip=$3 vm seen="" deadline

	pkill -f '[q]emu-system-x86_64' 2>/dev/null || true
	sleep 1
	mkdir -p "$SHARE"
	rm -f "$SHARE"/* 2>/dev/null || true

	echo river > "$SHARE/mode"
	touch "$SHARE/nofuzzel" # irrelevant here, and it steals keyboard focus
	[ "$csd" = yes ] && touch "$SHARE/csd"
	[ "$clip" = none ] || echo "$clip" > "$SHARE/clip"

	echo "== $name (decorations=$([ "$csd" = yes ] && echo client || echo server), clip=$clip)" >&2
	( cd "$REPO" && QEMU_OPTS="-display none" ./result/bin/run-nixos-vm ) \
		> "$OUT/$name-console.log" 2>&1 &
	vm=$!
	deadline=$((SECONDS + 300))

	while [ $SECONDS -lt $deadline ]; do
		kill -0 $vm 2>/dev/null || break
		for m in "$SHARE"/mark-*; do
			[ -e "$m" ] || continue
			local n
			n=$(basename "$m" | sed 's/^mark-//')
			case " $seen " in *" $n "*) continue ;; esac
			seen="$seen $n"
			if [ "$n" = one ]; then
				printf 'screendump %s\n' "$OUT/$name.ppm" \
					| socat_cmd - "UNIX-CONNECT:$MON" >/dev/null 2>&1
				sleep 0.8
				if [ -s "$OUT/$name.ppm" ]; then
					magick "$OUT/$name.ppm" "$OUT/$name.png" \
						&& rm -f "$OUT/$name.ppm"
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

# Percentage of a region that is the background colour, i.e. "how much of this
# was NOT painted". -sample, not -resize: nearest neighbour keeps exact colours
# where averaging would invent new ones.
bg_pct() {
	local img=$1 geom=$2 total bg
	local px
	px=$(magick "$img" -crop "$geom" +repage -sample 80x20! txt: 2>/dev/null | tail -n +2)
	total=$(printf '%s\n' "$px" | grep -c .)
	bg=$(printf '%s\n' "$px" | grep -c "$BG")
	[ "$total" -gt 0 ] || { echo 0; return; }
	echo $((bg * 100 / total))
}

rm -rf "$OUT"
mkdir -p "$OUT"
run baseline yes none
run clipped-csd yes full
run clipped-ssd no full

for f in baseline clipped-csd clipped-ssd; do
	[ -s "$OUT/$f.png" ] || {
		echo "missing $OUT/$f.png; see $OUT/$f-console.log" >&2
		exit 1
	}
done

W=$(magick "$OUT/baseline.png" -format '%w' info:)
title_geom="${W}x6+0+$((TITLE_ROW - 3))"
body_geom="${W}x${BODY_HEIGHT}+0+${BODY_TOP}"

echo
printf '%-14s %-12s %-8s %s\n' run decorations 'title' 'content'
printf '%-14s %-12s %-8s %s\n' --- ----------- ------- -------
for spec in "baseline client no" "clipped-csd client yes" "clipped-ssd server yes"; do
	set -- $spec
	t=$(bg_pct "$OUT/$1.png" "$title_geom")
	b=$(bg_pct "$OUT/$1.png" "$body_geom")
	printf '%-14s %-12s %3d%% bg  %3d%% bg\n' "$1" "$2" "$t" "$b"
	eval "${1//-/_}_title=$t; ${1//-/_}_body=$b"
done

# Label and stack the three, so the difference is one file rather than three.
#
# The labels carry the measurements, and the title band is outlined, because the
# eye-catching difference between the first two frames is the title bar changing
# from white to grey -- which is foot drawing it focused vs unfocused, varies
# between runs, and means nothing. The decisive comparison is the second frame
# against the third: same clip box, and only one of them still has a title bar.
# Captions are derived from the measurements, never asserted. An earlier version
# hardcoded "DECORATIONS SURVIVED" for the clipped run, so after the fix the
# image cheerfully reported the bug while the numbers beside it said otherwise.
strip_label() {
	local title=$2 body=$3 verdict

	case $1 in
	baseline)
		echo "1  no clip: decorations drawn ($title% bg), content drawn ($body% bg)"
		;;
	clipped-csd)
		if [ "$title" -lt 20 ]; then
			verdict="DECORATIONS SURVIVED  <- BUG"
		elif [ "$title" -ge 80 ]; then
			verdict="decorations clipped too  <- correct"
		else
			verdict="decorations partly clipped  <- unclear"
		fi
		echo "2  clipped, client-side decor: title $title% bg, content $body% bg   $verdict"
		;;
	clipped-ssd)
		echo "3  clipped, server-side decor: title $title% bg, content $body% bg   <- control, no subsurfaces"
		;;
	esac
}

# Which font, if any. magick resolves fonts differently depending on how the
# environment is set up, and a missing one aborts the whole command rather than
# just dropping the text -- so pick one that exists, and if none does, draw the
# strips unlabelled instead of producing no image at all.
FONT=$(fc-match -f '%{file}' monospace 2>/dev/null)
[ -n "${FONT:-}" ] && [ -r "$FONT" ] || FONT=

for spec in "baseline $baseline_title $baseline_body" \
	"clipped-csd $clipped_csd_title $clipped_csd_body" \
	"clipped-ssd $clipped_ssd_title $clipped_ssd_body"; do
	set -- $spec
	# Order matters: caption band first, then the box, then the text with the
	# stroke turned back off. Drawing the box before the splice puts it under
	# the caption, and leaving -stroke set outlines the text in red.
	if [ -n "$FONT" ]; then
		magick "$OUT/$1.png" -crop "${W}x400+0+0" +repage -resize 900x \
			-gravity north -background '#111111' -splice 0x22 \
			-gravity northwest \
			-fill none -stroke red -strokewidth 2 \
			-draw "rectangle 2,24 897,46" \
			-stroke none -fill white -font "$FONT" -pointsize 13 \
			-annotate +6+16 "$(strip_label "$1" "$2" "$3")" \
			"$OUT/strip-$1.png" 2>/dev/null
	fi
	[ -s "$OUT/strip-$1.png" ] || magick "$OUT/$1.png" \
		-crop "${W}x400+0+0" +repage -resize 900x \
		-fill none -stroke red -strokewidth 2 \
		-draw "rectangle 2,2 897,24" "$OUT/strip-$1.png" 2>/dev/null
done
magick "$OUT"/strip-baseline.png "$OUT"/strip-clipped-csd.png \
	"$OUT"/strip-clipped-ssd.png -append "$OUT/evidence.png" 2>/dev/null \
	&& rm -f "$OUT"/strip-*.png

echo
# The control has to hold, or the rest means nothing: a clip box must blank a
# window that has no subsurfaces.
if [ "$clipped_ssd_body" -lt 80 ] || [ "$clipped_ssd_title" -lt 80 ]; then
	echo "INCONCLUSIVE: the control did not clip. With server-side decorations"
	echo "the window has no subsurfaces, so the clip box should have blanked it"
	echo "entirely. Something other than subsurface handling is wrong."
	exit 0
fi

if [ "$clipped_csd_body" -ge 80 ] && [ "$clipped_csd_title" -lt 20 ]; then
	echo "BUG PRESENT: the clip box removed the window content but not its"
	echo "decorations. Same clip, same manager; the only difference from the"
	echo "control is that the client drew its own decorations, i.e. created"
	echo "subsurfaces. The box lives on the toplevel view and never reaches them."
	echo
	echo "Compare frames 2 and 3, not 1 and 2: both got the same clip box, and"
	echo "only one still has a title bar. Frame 1 is just what unclipped looks"
	echo "like. Ignore the title bar changing colour between 1 and 2 -- that is"
	echo "focused vs unfocused, it varies between runs, and it means nothing."
elif [ "$clipped_csd_body" -ge 80 ] && [ "$clipped_csd_title" -ge 80 ]; then
	echo "BUG FIXED: the clip box removed the decorations along with the content."
else
	echo "INCONCLUSIVE: the clipped run does not match either shape."
	echo "Look at $OUT/evidence.png."
fi

echo
echo "frames:   $OUT/{baseline,clipped-csd,clipped-ssd}.png"
echo "stacked:  $OUT/evidence.png"
