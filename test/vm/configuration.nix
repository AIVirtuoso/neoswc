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
    pkgs.libdrm # modetest
    pkgs.wayland-utils
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
      pkgs.libdrm
      pkgs.coreutils
      pkgs.procps
      pkgs.util-linux
      pkgs.gnugrep
      pkgs.gawk
      pkgs.iproute2
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

      say "dri devices: $(ls /dev/dri 2>/dev/null | tr '\n' ' ')"
      if [ ! -e /dev/dri/card0 ]; then
        say "FAIL no DRM device; virtio-gpu did not bind"
        exit 1
      fi
      say "kms: $(modetest -M virtio_gpu -c 2>/dev/null | grep -c '^[0-9]') connector line(s)"

      # The Zig window manager cannot be built inside the nix sandbox (its
      # translate-c dependency needs network), so it is built on the host and
      # dropped into the shared directory. The share is mounted at the same
      # path it has on the host precisely so the binary's rpath resolves in
      # both places -- LD_LIBRARY_PATH is not an option, since swc-launch is
      # setuid and the loader strips it before the manager is spawned.
      WM=neoswc-example-wm
      if [ -x /tmp/neoswc-vm-share/zig-wm ]; then
        WM=/tmp/neoswc-vm-share/zig-wm
        say "using the Zig window manager"
      elif [ "$(cat /tmp/neoswc-vm-share/mode 2>/dev/null)" = river ]; then
        # neoswc does no window management itself: it serves
        # river-window-management-v1 and a separate client decides the layout.
        WM=neoswc
        say "using neoswc with the river protocol"
      fi

      say "starting compositor"
      # Never wait on it: it has blocked uninterruptibly in past runs, and a
      # blocked child must not take the diagnostics down with it.
      # setsid: swc-launch takes over the VT and changes its foreground process
      # group. Without a separate session the kernel sends SIGTTOU to this
      # script, which stops it -- it goes quiet without dying, which is exactly
      # what happened before.
      setsid /run/wrappers/bin/swc-launch -n -t /dev/tty1 -- "$WM" \
        > /tmp/neoswc.log 2>&1 </dev/null &
      sleep 8

      say "processes: $(ps -eo pid,ppid,stat,comm | grep -Ei 'swc-launch|neoswc-example' | tr '\n' ';')"

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

      # In river mode the layout comes from a protocol client, so it has to be
      # running before any window appears.
      if [ "$WM" = neoswc ]; then
        setsid river-wm-client > /tmp/wmclient.log 2>&1 &
        sleep 2
        say "wm client: $(head -2 /tmp/wmclient.log 2>/dev/null | tr '\n' '|')"
      fi

      # The example wm grid-tiles on every add, so each new client triggers a
      # multi-window relayout -- the exact operation the barrier makes atomic.
      # Keep their output: a client that dies silently is the whole problem.
      setsid foot ${pkgs.coreutils}/bin/sleep 3600 > /tmp/foot1.log 2>&1 &
      sleep 4
      say "MARK one-window (foot1: $(ps -eo comm | grep -c '^foot$') alive)"
      touch /tmp/neoswc-vm-share/mark-one

      setsid foot ${pkgs.coreutils}/bin/sleep 3600 > /tmp/foot2.log 2>&1 &
      sleep 4
      say "MARK two-windows (foot2: $(ps -eo comm | grep -c '^foot$') alive)"
      touch /tmp/neoswc-vm-share/mark-two

      # Straggler path. Wedge a client with SIGSTOP so it cannot acknowledge,
      # then force a relayout by adding another window. The cohort must give up
      # on it and show everyone else rather than blocking -- the barrier's
      # defining behaviour, and until now only covered by unit tests.
      victim=$(pgrep -x foot | head -1)
      kill -STOP "$victim"
      say "STOPPED client $victim"

      setsid foot ${pkgs.coreutils}/bin/sleep 3600 > /tmp/foot3.log 2>&1 &
      sleep 4
      say "MARK three-windows-one-wedged"
      touch /tmp/neoswc-vm-share/mark-three
      wait_host three

      # ...and it must recover once the client comes back, not stay degraded.
      kill -CONT "$victim"
      say "RESUMED client $victim"

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
      say "state requests: $(grep -c '^window: ' /tmp/neoswc.log 2>/dev/null)"
      say "$(grep '^window: ' /tmp/neoswc.log 2>/dev/null | tr '\n' '|')"
      # The whole point: did the cohort actually run, and did it complete
      # rather than time out? A screenshot cannot tell these apart.
      say "relayouts: $(grep -cE "^(arrange|zig-wm): relayout" /tmp/neoswc.log 2>/dev/null)"
      if [ -s /tmp/wmclient.log ]; then
        say "wm client sequences: manage=$(grep -c 'manage_start:' /tmp/wmclient.log) render=$(grep -c 'render_start:' /tmp/wmclient.log)"
        # Reported separately: the output events arrive first and were being
        # cut off by the tail below.
        say "wm client outputs: $(grep -E '^wmclient: (new output|output )' /tmp/wmclient.log | tr '\n' '|')"
        say "wm client log: $(tr '\n' '|' < /tmp/wmclient.log | tail -c 1200)"
      fi
      say "$(grep '^arrange: relayout' /tmp/neoswc.log 2>/dev/null | tr '\n' '|')"
      say "foot1 log: $(cat /tmp/foot1.log 2>/dev/null | tr '\n' '|')"
      say "foot2 log: $(cat /tmp/foot2.log 2>/dev/null | tr '\n' '|')"
      say "READY"
      touch /tmp/neoswc-vm-share/ready
      sync

      # Hold while the host screendumps; it drops a 'quit' file when finished.
      for i in $(seq 1 120); do
        [ -e /tmp/neoswc-vm-share/quit ] && break
        sleep 1
      done

      say "log: $(cat /tmp/neoswc.log 2>/dev/null | tr '\n' '|')"
      say "DONE"
      sync
      systemctl poweroff -f || poweroff -f
    '';
  };
}
