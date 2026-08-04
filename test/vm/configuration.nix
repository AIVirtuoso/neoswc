# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 neoswc contributors
#
# Guest configuration for the neoswc test VM.
#
# swc has no nested or headless backend: drm.c is the only output path and it
# wants DRM master on a real VT. A QEMU guest supplies exactly that, so the
# compositor runs completely unmodified -- no test-only code paths, which is
# the point. virtio-gpu has no acceleration, but wld falls back to its generic
# dumb-buffer driver (wld drm.c: "Falling back to dumb DRM driver"), so this
# needs no GPU passthrough.
#
# The guest drives itself and reports to the serial console; the host reads
# console.log and pulls frames with QEMU's screendump. See CLAUDE.md.
{
  pkgs,
  lib,
  neoswc,
  ...
}:
let
  # fuzzel segfaults against this compositor on real hardware, inside its own
  # wayl_refresh(). A stripped backtrace only reached the nearest symbol, so
  # build it with DWARF: the guest is where the crash can be caught with a core
  # and locals rather than inferred.
  fuzzel-dbg = pkgs.fuzzel.overrideAttrs (o: {
    mesonBuildType = "debugoptimized";
    # Assertions left on deliberately: fuzzel asserts both that
    # zxdg_output_manager_v1 was advertised before wl_output and that the
    # output scale it ends up with is >= 1. Those are the two things the
    # compositor was getting wrong, so an assertion build is the stricter test.
    dontStrip = true;
    separateDebugInfo = false;
  });
in
{
  system.stateVersion = "26.05";

  virtualisation = {
    memorySize = 2048;
    cores = 2;
    diskSize = 4096;

    # false gives a serial console on stdio rather than a host window. The GPU
    # is added explicitly below -- swc needs a DRM device even though nothing
    # on the host displays it.
    graphics = false;

    # swc-launch puts the VT into KD_GRAPHICS, after which the service's console
    # output stops arriving even though the process is still running. Diagnostics
    # go to a host directory instead, which survives a wedged guest.
    # Create it on the host before booting: mkdir -p /tmp/neoswc-vm-share
    sharedDirectories.out = {
      source = "/tmp/neoswc-vm-share";
      target = "/tmp/neoswc-vm-share";
    };

    qemu.options = [
      # -nographic does NOT remove QEMU's default VGA. Without this the guest
      # gets two DRM devices, and find_primary_drm_device() in swc's drm.c
      # prefers the boot_vga one -- i.e. the wrong card. It also makes
      # screendump capture the text console instead of the compositor.
      "-vga none"
      "-device virtio-gpu-pci"
      # For screendump, and for driving the guest from the host.
      "-monitor unix:/tmp/neoswc-vm-monitor.sock,server,nowait"
      # QMP as well: the human monitor's mouse_move sends relative deltas,
      # which the absolute usb-tablet ignores. input-send-event with abs axes
      # is the only way to actually move the guest cursor.
      "-qmp unix:/tmp/neoswc-vm-qmp.sock,server,nowait"
    ];
  };

  boot = {
    initrd.availableKernelModules = [ "virtio_gpu" ];
    kernelParams = [ "console=ttyS0" "loglevel=4" ];
  };

  users.users.root.password = "";
  services.getty.autologinUser = "root";

  environment.systemPackages = [
    neoswc
    pkgs.foot
    fuzzel-dbg
    pkgs.libdrm # modetest
    pkgs.wayland-utils
    # A background layer surface, so the area behind a window is not the
    # compositor's black clear colour. Without one, anything the compositor
    # fails to paint is invisible against a black screen.
    pkgs.swaybg
  ];

  # swc-launch opens DRM devices and manages the VT, so it must be setuid.
  security.wrappers.swc-launch = {
    setuid = true;
    owner = "root";
    group = "root";
    source = "${neoswc}/bin/swc-launch";
  };

  documentation.enable = false;

  # foot refuses to start without a monospace font, and a minimal guest ships
  # none. This is the difference between two windows and zero.
  fonts.packages = [ pkgs.dejavu_fonts ];

  # Everything the host needs to know is printed to the console with SMOKE:
  # markers, so a run is judged by reading console.log rather than by poking at
  # an interactive shell.
  systemd.services.neoswc-smoke = {
    description = "neoswc smoke test";
    wantedBy = [ "multi-user.target" ];
    after = [ "systemd-user-sessions.service" ];
    serviceConfig = {
      Type = "simple";
      StandardOutput = "journal+console";
      StandardError = "journal+console";
      Restart = "no";
    };
    path = [
      neoswc
      pkgs.foot
      fuzzel-dbg
      pkgs.libdrm
      pkgs.coreutils
      pkgs.procps
      pkgs.util-linux
      pkgs.gnugrep
      pkgs.gawk
      pkgs.iproute2
      pkgs.gdb
      pkgs.wev
      pkgs.wlr-randr
      pkgs.wayland-utils
      pkgs.swaybg
    ];
    script = ''
      OUT=/tmp/neoswc-vm-share/smoke.log
      : > "$OUT" 2>/dev/null || true
      # Console output stops once the VT goes graphical, so everything is
      # mirrored to the shared directory, which is what the host actually reads.
      say() { echo "SMOKE: $*"; echo "SMOKE: $*" >> "$OUT" 2>/dev/null || true; }

      # Block until the host has captured this state, so screendumps land on the
      # state they name instead of whatever the guest raced ahead to. Proceeds
      # anyway after 30s so an unattended run still finishes.
      wait_host() {
        for _ in $(seq 1 60); do
          [ -e "/tmp/neoswc-vm-share/go-$1" ] && return 0
          sleep 0.5
        done
      }

      # NOT /run/user/0. logind mounts a fresh tmpfs there when root autologs in
      # on tty1, which shadows whatever was already in the directory: the
      # compositor binds its socket first, then the socket becomes unreachable
      # by path while ss still reports it. Use a directory logind never touches.
      export XDG_RUNTIME_DIR=/run/neoswc
      mkdir -p "$XDG_RUNTIME_DIR"
      chmod 700 "$XDG_RUNTIME_DIR"

      # rill reads $XDG_CONFIG_HOME/rill/config.zon, else $HOME/.config/rill.
      # Without one it has no keybindings at all, so testing bindings against
      # rill means giving it the real config from the host.
      export HOME=/root
      if [ -s /tmp/neoswc-vm-share/rill-config ]; then
        mkdir -p /root/.config/rill
        cp /tmp/neoswc-vm-share/rill-config /root/.config/rill/config.zon
        say "installed rill config ($(wc -l < /root/.config/rill/config.zon) lines)"
      fi

      # fuzzel's own config decides whether its background is translucent, which
      # is the difference between "the compositor cannot blend" and "the client
      # asked for an opaque window". Take the host's when one is provided.
      if [ -s /tmp/neoswc-vm-share/fuzzel-config ]; then
        mkdir -p /root/.config/fuzzel
        cp /tmp/neoswc-vm-share/fuzzel-config /root/.config/fuzzel/fuzzel.ini
        say "installed fuzzel config ($(wc -l < /root/.config/fuzzel/fuzzel.ini) lines)"
      fi

      # Force foot to draw its own decorations, which it does with subsurfaces
      # (csd_instantiate() in its wayland.c) -- a title bar, buttons and borders,
      # each its own wl_surface. With server-side decorations foot creates none,
      # so nothing in a default run has a subsurface to clip and the gap is
      # invisible. Combine with NEOSWC_TEST_CLIP to see it.
      if [ -e /tmp/neoswc-vm-share/csd ]; then
        export NEOSWC_TEST_CSD=1
        mkdir -p /root/.config/foot
        printf '[csd]\npreferred=client\n' > /root/.config/foot/foot.ini
        say "foot: client-side decorations forced (subsurfaces)"
      fi

      # river-wm-client clips its first window to 1x1 when this is set, so a
      # screendump against an unclipped baseline shows whether clipping reaches
      # the framebuffer at all.
      if [ -s /tmp/neoswc-vm-share/clip ]; then
        export NEOSWC_TEST_CLIP=$(cat /tmp/neoswc-vm-share/clip)
        say "clip test mode: $NEOSWC_TEST_CLIP"
      fi

      say "dri devices: $(ls /dev/dri 2>/dev/null | tr '\n' ' ')"
      if [ ! -e /dev/dri/card0 ]; then
        say "FAIL no DRM device; virtio-gpu did not bind"
        exit 1
      fi
      say "kms: $(modetest -M virtio_gpu -c 2>/dev/null | grep -c '^[0-9]') connector line(s)"
      say "input devices: $(ls /dev/input 2>/dev/null | tr '\n' ' ')"

      # The Zig window manager cannot be built inside the nix sandbox (its
      # translate-c dependency needs network), so it is built on the host and
      # dropped into the shared directory. The share is mounted at the same
      # path it has on the host precisely so the binary's rpath resolves in
      # both places -- LD_LIBRARY_PATH is not an option, since swc-launch is
      # setuid and the loader strips it before the manager is spawned.
      WM=neoswc-example-wm
      CLIENT=
      if [ -x /tmp/neoswc-vm-share/zig-wm ]; then
        WM=/tmp/neoswc-vm-share/zig-wm
        say "using the Zig window manager"
      elif [ "$(cat /tmp/neoswc-vm-share/mode 2>/dev/null)" = river ]; then
        # neoswc does no window management itself: it serves
        # river-window-management-v1 and a separate client decides the layout.
        # That client is passed to neoswc, which spawns it once its socket
        # exists -- it cannot be handed to swc-launch, whose argument is the
        # compositor. A real window manager can be substituted by writing its
        # path into the shared directory; the guest shares the host's nix
        # store, so a host store path resolves here.
        WM=neoswc
        CLIENT=river-wm-client
        if [ -s /tmp/neoswc-vm-share/wmclient ]; then
          CLIENT=$(cat /tmp/neoswc-vm-share/wmclient)
          say "wm client override: $CLIENT"
        fi
        say "using neoswc with the river protocol, manager $CLIENT"
      fi

      # Cores land in the shared directory so gdb can be run on them after the
      # fact. The crashing process's cwd is /, so the pattern must be absolute.
      ulimit -c unlimited || true
      echo '/tmp/neoswc-vm-share/core.%e.%p' > /proc/sys/kernel/core_pattern 2>/dev/null || true

      say "starting compositor"
      # Never wait on it: it has blocked uninterruptibly in past runs, and a
      # blocked child must not take the diagnostics down with it.
      # setsid: swc-launch takes over the VT and changes its foreground process
      # group. Without a separate session the kernel sends SIGTTOU to this
      # script, which stops it -- it goes quiet without dying, which is exactly
      # what happened before.
      setsid /run/wrappers/bin/swc-launch -n -t /dev/tty1 -- "$WM" $CLIENT \
        > /tmp/neoswc.log 2>&1 </dev/null &
      sleep 8

      # The manager is a child of the compositor now, so its output lands in
      # the compositor's log. Keep splitting it back out, because every check
      # below reads /tmp/wmclient.log and river-wm-client prefixes its lines.
      if [ -n "$CLIENT" ]; then
        ( while true; do
            grep '^wmclient:' /tmp/neoswc.log > /tmp/wmclient.log 2>/dev/null || true
            sleep 1
          done ) &
      fi

      say "processes: $(ps -eo pid,ppid,stat,comm | grep -Ei 'swc-launch|neoswc' | tr '\n' ';')"

      # The compositor was seen in do_epoll_wait, which is past
      # wl_display_add_socket_auto() -- yet no socket exists on disk. Ask the
      # process itself rather than inferring: what environment did it get, and
      # what is it actually listening on.
      # -f, not -x: comm is truncated to 15 chars ("neoswc-example-").
      wmpid=$(pgrep -f neoswc-example-wm | head -1)
      if [ -n "$wmpid" ]; then
        say "wm pid=$wmpid env: $(tr '\0' '\n' < /proc/$wmpid/environ 2>/dev/null | grep -E '^(XDG_RUNTIME_DIR|WAYLAND_DISPLAY|HOME|USER)=' | tr '\n' ' ')"
        say "wm cwd=$(readlink /proc/$wmpid/cwd 2>/dev/null) root=$(readlink /proc/$wmpid/root 2>/dev/null)"
        say "wm sockets: $(ls -l /proc/$wmpid/fd 2>/dev/null | grep -c socket) fd(s)"
      else
        say "wm process not found by name"
      fi
      say "listening unix sockets: $(ss -lxH 2>/dev/null | awk '{print $5}' | grep -i wayland | tr '\n' ' ')"
      say "all wayland-ish paths: $(find / -xdev -maxdepth 5 -name 'wayland-*' 2>/dev/null | tr '\n' ' ')"

      sock=""
      for i in $(seq 1 50); do
        sock=$(ls "$XDG_RUNTIME_DIR"/wayland-* 2>/dev/null | grep -v '\.lock$' | head -1)
        [ -n "$sock" ] && break
        sleep 0.2
      done

      if [ -z "$sock" ]; then
        say "FAIL no wayland socket"
        say "XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR contents: $(ls -a "$XDG_RUNTIME_DIR" 2>&1 | tr '\n' ' ')"
        say "sockets anywhere: $(find /run /tmp -maxdepth 3 -name 'wayland-*' 2>/dev/null | tr '\n' ' ')"
        say "log: $(cat /tmp/neoswc.log 2>/dev/null | tr '\n' '|')"
        touch /tmp/neoswc-vm-share/failed
        sync
        systemctl poweroff -f || poweroff -f
        exit 1
      fi

      export WAYLAND_DISPLAY="$(basename "$sock")"
      say "compositor up on $WAYLAND_DISPLAY"

      # The manager was spawned by the compositor, before this point. Report
      # what it did rather than starting it: a manager that failed to exec is
      # the difference between a tiled screen and a blank one.
      if [ -n "$CLIENT" ]; then
        sleep 3
        say "manager spawn: $(grep -E '^neoswc: (spawned|failed to exec|manager)' /tmp/neoswc.log 2>/dev/null | tr '\n' '|')"
        say "wm client: $(head -4 /tmp/wmclient.log 2>/dev/null | tr '\n' '|')"
        say "wm client alive: $(pgrep -fc "$CLIENT" 2>/dev/null || true)"
      fi

      # Output management: is the global there, and does moving a head actually
      # take effect? The guest has one screen, so this cannot check a two-monitor
      # arrangement -- it checks that kanshi has something to talk to and that an
      # applied configuration reaches swc_screen_set_position().
      say "output mgmt global: $(wayland-info 2>/dev/null | grep -c zwlr_output_manager_v1 || true)"
      if command -v wlr-randr >/dev/null 2>&1; then
        say "wlr-randr before: $(wlr-randr 2>&1 | tr '\n' '|' | head -c 300)"
        wlr-randr --output Virtual-1 --pos 300,150 > /tmp/wlrrandr.log 2>&1 || true
        sleep 1
        say "wlr-randr apply: $(cat /tmp/wlrrandr.log | tr '\n' '|' | head -c 200)"
        say "wlr-randr after: $(wlr-randr 2>&1 | grep -i position | tr '\n' '|' | head -c 200)"
        # Put it back. Leaving the screen at 300,150 offsets every screendump
        # taken later in the run, which looks like a rendering fault in whatever
        # is being tested next rather than like this check not cleaning up.
        wlr-randr --output Virtual-1 --pos 0,0 >/dev/null 2>&1 || true
        sleep 1
        say "wlr-randr restored: $(wlr-randr 2>&1 | grep -i position | tr '\n' '|' | head -c 120)"
      fi

      # A solid-colour background layer surface. The compositor clears to black,
      # so without this every region it fails to paint is black on black and
      # cannot be told from empty desktop. Magenta is in nothing else on screen.
      if [ ! -e /tmp/neoswc-vm-share/nobg ]; then
        setsid swaybg -c '#ff00ff' > /tmp/swaybg.log 2>&1 &
        sleep 2
        say "swaybg alive: $(pgrep -c -x swaybg || true)"
      fi

      # The example wm grid-tiles on every add, so each new client triggers a
      # multi-window relayout -- the exact operation the barrier makes atomic.
      # Keep their output: a client that dies silently is the whole problem.
      setsid foot ${pkgs.coreutils}/bin/sleep 3600 > /tmp/foot1.log 2>&1 &
      sleep 4
      say "MARK one-window (foot1: $(ps -eo comm | grep -c '^foot$') alive)"
      touch /tmp/neoswc-vm-share/mark-one

      # fuzzel is a layer-shell client and it segfaults against this compositor
      # on real hardware. Run it here, with one window already up, and get out
      # of the way again: it takes keyboard focus, which would otherwise wreck
      # the focus and binding measurements later in this script.
      # Skippable: fuzzel maps a layer surface and takes keyboard focus, so when
      # something later in the run misbehaves it is worth being able to take it
      # out of the picture rather than reasoning about whether it mattered.
      if [ -e /tmp/neoswc-vm-share/nofuzzel ]; then
        say "fuzzel: skipped on request"
      else
      printf 'alpha\nbeta\ngamma\n' | setsid env WAYLAND_DEBUG=1 \
        fuzzel --dmenu --log-level=debug --log-no-syslog \
        > /tmp/fuzzel.log 2>&1 &
      # The reported artefact -- a black frame around fuzzel -- lasts about a
      # second, so the settled screendump below is far too late to catch it.
      # Hand the host the moment fuzzel is launched and let it burst-capture.
      say "MARK fuzzel-spawn"
      touch /tmp/neoswc-vm-share/mark-fuzzel-spawn
      wait_host fuzzel-spawn
      sleep 5
      say "fuzzel alive: $(pgrep -c -x fuzzel || true)"
      say "MARK fuzzel"
      touch /tmp/neoswc-vm-share/mark-fuzzel
      wait_host fuzzel
      pkill -x fuzzel 2>/dev/null || true
      sleep 2
      # Immediately after the layer surface goes away and before anything else
      # happens: the screen has gone blank here in past runs, and every other
      # mark is far enough downstream that a third window has already been added
      # and the cause cannot be told apart from a relayout.
      say "MARK fuzzel-gone (foot: $(ps -eo comm | grep -c '^foot$') alive)"
      touch /tmp/neoswc-vm-share/mark-fuzzel-gone
      wait_host fuzzel-gone
      cp /tmp/fuzzel.log /tmp/neoswc-vm-share/fuzzel.log 2>/dev/null || true
      say "fuzzel own log: $(grep -vE '^\[[0-9]+\.[0-9]+\]' /tmp/fuzzel.log 2>/dev/null | tail -c 1500 | tr '\n' '|')"
      say "fuzzel wire tail: $(tail -c 2500 /tmp/fuzzel.log 2>/dev/null | tr '\n' '|')"
      say "fuzzel crash: $(dmesg | grep -iE 'fuzzel.*(segfault|trap)' | tail -2 | tr '\n' '|')"
      fcore=$(ls -t /tmp/neoswc-vm-share/core.fuzzel.* 2>/dev/null | head -1)
      if [ -n "$fcore" ]; then
        say "fuzzel backtrace:"
        gdb -batch -n -ex 'set pagination off' -ex 'bt full' \
          "$(command -v fuzzel)" "$fcore" 2>&1 \
          | grep -vE '^\[|^Using host|^Core was|warning:' | head -60 \
          | while read -r l; do say "  $l"; done
      else
        say "fuzzel: no core file"
      fi
      fi

      setsid foot ${pkgs.coreutils}/bin/sleep 3600 > /tmp/foot2.log 2>&1 &
      sleep 4
      say "MARK two-windows (foot2: $(ps -eo comm | grep -c '^foot$') alive)"
      touch /tmp/neoswc-vm-share/mark-two

      # Straggler path. Wedge a client with SIGSTOP so it cannot acknowledge,
      # then force a relayout by adding another window. The cohort must give up
      # on it and show everyone else rather than blocking -- the barrier's
      # defining behaviour, and until now only covered by unit tests.
      # Guarded: the script runs under set -e, so kill with an empty pid aborts
      # it before the diagnostics below are ever printed. That happens whenever
      # the clients failed to start, which is exactly when the log is wanted.
      victim=$(pgrep -x foot | head -1 || true)
      if [ -n "$victim" ]; then
        kill -STOP "$victim" || true
        say "STOPPED client $victim"
      else
        say "no client to wedge; skipping the straggler check"
      fi

      setsid foot ${pkgs.coreutils}/bin/sleep 3600 > /tmp/foot3.log 2>&1 &
      sleep 4
      say "MARK three-windows-one-wedged"
      touch /tmp/neoswc-vm-share/mark-three
      wait_host three

      # ...and it must recover once the client comes back, not stay degraded.
      if [ -n "$victim" ]; then
        kill -CONT "$victim" || true
        say "RESUMED client $victim"
      fi

      setsid foot ${pkgs.coreutils}/bin/sleep 3600 > /tmp/foot4.log 2>&1 &
      sleep 4
      say "MARK four-windows-recovered"
      touch /tmp/neoswc-vm-share/mark-four
      wait_host four

      # A client that asks to be maximized on startup, to confirm the
      # xdg_toplevel state requests reach the window manager. This wm tiles, so
      # it only reports them.
      setsid foot --maximized ${pkgs.coreutils}/bin/sleep 3600 > /tmp/foot5.log 2>&1 &
      sleep 4
      say "MARK maximize-request"
      say "foot5 log: $(cat /tmp/foot5.log 2>/dev/null | tr '\n' '|')"

      say "clients: $(ps -eo comm | grep -c '^foot$') foot process(es)"
      say "compositor alive: $(pgrep -c -x neoswc || true)"
      say "crash: $(dmesg | grep -iE 'segfault|general protection|trap ' | tail -3 | tr '\n' '|')"
      core=$(ls -t /tmp/neoswc-vm-share/core.neoswc* 2>/dev/null | head -1)
      if [ -n "$core" ]; then
        say "backtrace:"
        gdb -batch -n -ex "bt" "$(command -v neoswc)" "$core" 2>&1 | grep -E "^#" | head -20 | while read -r l; do say "  $l"; done
      else
        say "no core file"
      fi
      say "state requests: $(grep -c '^window: ' /tmp/neoswc.log 2>/dev/null)"
      say "$(grep '^window: ' /tmp/neoswc.log 2>/dev/null | tr '\n' '|')"
      # The whole point: did the cohort actually run, and did it complete
      # rather than time out? A screenshot cannot tell these apart.
      say "compositor log: $(tr '\n' '|' < /tmp/neoswc.log | tail -c 1500)"
      say "relayouts: $(grep -cE "^(arrange|zig-wm): relayout" /tmp/neoswc.log 2>/dev/null)"
      if [ -s /tmp/wmclient.log ]; then
        say "wm client sequences: manage=$(grep -c 'manage_start:' /tmp/wmclient.log) render=$(grep -c 'render_start:' /tmp/wmclient.log)"
        # Reported separately: the output events arrive first and were being
        # cut off by the tail below.
        say "windows seen by manager: $(grep -c 'new window' /tmp/wmclient.log 2>/dev/null || true)"
      say "wm client outputs: $(grep -E '^wmclient: (new output|output )' /tmp/wmclient.log | tr '\n' '|')"
        say "wm client seat: $(grep -E '^wmclient: (new seat|seat )' /tmp/wmclient.log | tr '\n' '|')"
        say "wm client bindings: $(grep -E '^wmclient: (registered|BINDING|bound river_xkb)' /tmp/wmclient.log | tr '\n' '|')"
        say "wm client log: $(tr '\n' '|' < /tmp/wmclient.log | tail -c 1200)"
        say "wm client tail: $(tail -c 900 /tmp/wmclient.log | tr '\n' '|')"
      fi
      say "$(grep '^arrange: relayout' /tmp/neoswc.log 2>/dev/null | tr '\n' '|')"
      say "foot1 log: $(cat /tmp/foot1.log 2>/dev/null | tr '\n' '|')"
      say "foot2 log: $(cat /tmp/foot2.log 2>/dev/null | tr '\n' '|')"
      # Two event-reporting clients. The manager focuses the newest window, so
      # wev2 should receive keys and wev1 should not -- that is the focus test.
      setsid stdbuf -oL -eL wev > /tmp/wev1.log 2>&1 &
      sleep 3
      setsid stdbuf -oL -eL wev > /tmp/wev2.log 2>&1 &
      sleep 3
      say "wev clients: $(pgrep -c -x wev || true)"

      say "READY"
      touch /tmp/neoswc-vm-share/ready
      sync

      # Hold while the host screendumps; it drops a 'quit' file when finished.
      for i in $(seq 1 120); do
        [ -e /tmp/neoswc-vm-share/quit ] && break
        sleep 1
      done

      # After the hold, so keys injected by the host in the meantime are seen.
      # Compositor-side view, which is the only view when the manager is rill:
      # what it asked swc for, and what actually fired.
      # After the hold, so a binding action the host triggered is visible. rill
      # binds Super+q to close_window, so the count dropping is the proof that
      # the manager acted -- not merely that the event was delivered.
      # rill binds Super+q to close_window and closes the *focused* window,
      # which is the newest -- a wev, not a foot. Count the windows the
      # compositor tore down instead of guessing which client it was.
      say "windows closed: $(grep -c 'Finalizing window' /tmp/neoswc.log 2>/dev/null || true)"
      say "clients after bindings: $(ps -eo comm | grep -c '^foot$') foot, $(pgrep -c -x wev || true) wev"
      # Key bindings say "enabled" and pointer bindings say "registered": key
      # matching moved into wm/, so nothing is handed to swc_add_binding for a
      # key any more. Counting only "registered" silently reported 2 instead of
      # 57 and looked like the bindings had stopped working.
      say "bindings registered: $(grep -cE '^neoswc: (key binding enabled|pointer binding registered)' /tmp/neoswc.log 2>/dev/null || true)"
      say "bindings fired: $(grep -c 'binding 0x.* fired' /tmp/neoswc.log 2>/dev/null || true)"
      say "binding fire log: $(grep 'fired' /tmp/neoswc.log 2>/dev/null | tail -8 | tr '\n' '|')"
      say "binding events: $(grep -c 'BINDING pressed' /tmp/wmclient.log 2>/dev/null || true) press(es)"
      say "binding log: $(grep '^wmclient: BINDING' /tmp/wmclient.log 2>/dev/null | tr '\n' '|')"
      say "wev1 log: $(tail -c 400 /tmp/wev1.log 2>/dev/null | tr '\n' ';')"
      say "focus enter: wev1=$(grep -c '] enter' /tmp/wev1.log 2>/dev/null || true) wev2=$(grep -c '] enter' /tmp/wev2.log 2>/dev/null || true)"
      say "focus: wev1 keys=$(grep -c '] key:' /tmp/wev1.log 2>/dev/null || true) wev2 keys=$(grep -c '] key:' /tmp/wev2.log 2>/dev/null || true)"
      say "pointer: $(grep -cE 'POINTER (enter|leave)' /tmp/wmclient.log 2>/dev/null || true) event(s); $(grep -E '^wmclient: POINTER' /tmp/wmclient.log 2>/dev/null | tr '\n' '|')"
      say "pointer binding: $(grep -cE 'PBINDING' /tmp/wmclient.log 2>/dev/null || true) event(s)"
      say "shell surface: $(grep -E '^wmclient: (SHELL|FAIL shell)' /tmp/wmclient.log 2>/dev/null | tr '\n' ';')"
      # Whole file, not a say() line: the manager is a child of the compositor
      # now, so its output is in here, and say() truncates.
      cp /tmp/neoswc.log /tmp/neoswc-vm-share/neoswc.log 2>/dev/null || true
      say "log: $(cat /tmp/neoswc.log 2>/dev/null | tr '\n' '|')"
      say "DONE"
      sync
      systemctl poweroff -f || poweroff -f
    '';
  };
}
