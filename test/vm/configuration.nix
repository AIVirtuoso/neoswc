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
      target = "/mnt/out";
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
      OUT=/mnt/out/smoke.log
      : > "$OUT" 2>/dev/null || true
      # Console output stops once the VT goes graphical, so everything is
      # mirrored to the shared directory, which is what the host actually reads.
      say() { echo "SMOKE: $*"; echo "SMOKE: $*" >> "$OUT" 2>/dev/null || true; }

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

      say "starting compositor"
      # Never wait on it: it has blocked uninterruptibly in past runs, and a
      # blocked child must not take the diagnostics down with it.
      # setsid: swc-launch takes over the VT and changes its foreground process
      # group. Without a separate session the kernel sends SIGTTOU to this
      # script, which stops it -- it goes quiet without dying, which is exactly
      # what happened before.
      setsid /run/wrappers/bin/swc-launch -n -t /dev/tty1 -- neoswc-example-wm \
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
        touch /mnt/out/failed
        sync
        systemctl poweroff -f || poweroff -f
        exit 1
      fi

      export WAYLAND_DISPLAY="$(basename "$sock")"
      say "compositor up on $WAYLAND_DISPLAY"

      # The example wm grid-tiles on every add, so each new client triggers a
      # multi-window relayout -- the exact operation the barrier makes atomic.
      # Keep their output: a client that dies silently is the whole problem.
      setsid foot ${pkgs.coreutils}/bin/sleep 3600 > /tmp/foot1.log 2>&1 &
      sleep 4
      say "MARK one-window (foot1: $(ps -eo comm | grep -c '^foot$') alive)"
      touch /mnt/out/mark-one

      setsid foot ${pkgs.coreutils}/bin/sleep 3600 > /tmp/foot2.log 2>&1 &
      sleep 4
      say "MARK two-windows (foot2: $(ps -eo comm | grep -c '^foot$') alive)"
      touch /mnt/out/mark-two

      say "clients: $(ps -eo comm | grep -c '^foot$') foot process(es)"
      # The whole point: did the cohort actually run, and did it complete
      # rather than time out? A screenshot cannot tell these apart.
      say "relayouts: $(grep -c '^arrange: relayout' /tmp/neoswc.log 2>/dev/null)"
      say "$(grep '^arrange: relayout' /tmp/neoswc.log 2>/dev/null | tr '\n' '|')"
      say "foot1 log: $(cat /tmp/foot1.log 2>/dev/null | tr '\n' '|')"
      say "foot2 log: $(cat /tmp/foot2.log 2>/dev/null | tr '\n' '|')"
      say "READY"
      touch /mnt/out/ready
      sync

      # Hold while the host screendumps; it drops a 'quit' file when finished.
      for i in $(seq 1 120); do
        [ -e /mnt/out/quit ] && break
        sleep 1
      done

      say "log: $(cat /tmp/neoswc.log 2>/dev/null | tr '\n' '|')"
      say "DONE"
      sync
      systemctl poweroff -f || poweroff -f
    '';
  };
}
