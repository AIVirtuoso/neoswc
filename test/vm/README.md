# neoswc test VM

swc has **no nested or headless backend**. `drm.c` is the only output path and
`libswc/meson.build` only chooses between libinput/evdev/wscons seats, so the
compositor wants DRM master on a real VT. That makes it impossible to exercise
from inside an existing Wayland session, and running it on a spare VT on the
host takes the screen over.

A QEMU/KVM guest supplies a real DRM device and real VTs, so neoswc runs
**completely unmodified** — no test-only code paths, which is the point of doing
it this way rather than adding a fake backend.

## Running

```bash
mkdir -p /tmp/neoswc-vm-share          # required: the guest's report channel
nix build .#vm
rm -f /tmp/neoswc-vm-share/*
QEMU_OPTS="-display none" ./result/bin/run-nixos-vm > console.log 2>&1
cat /tmp/neoswc-vm-share/smoke.log
```

The guest runs `systemd.services.neoswc-smoke`, which reports progress as
`SMOKE:` lines and powers the machine off when finished, so a run is a bounded
foreground command.

To capture frames, boot it in the background and talk to the monitor:

```bash
printf 'screendump /tmp/frame.ppm\n' | socat - UNIX-CONNECT:/tmp/neoswc-vm-monitor.sock
magick /tmp/frame.ppm /tmp/frame.png
```

The guest waits (up to 120s) for `/tmp/neoswc-vm-share/quit` before shutting
down, so the host has time to screendump. `touch` that file to release it.

## Status

Working:

- Guest boots with a single virtio-gpu DRM device and a KMS connector.
- `swc-launch` acquires the VT and DRM (`running on /dev/tty1`).
- The compositor process reaches `wl_display_run()` — observed sitting in
  `do_epoll_wait`.
- `nix build .#neoswc` runs the transaction unit tests inside the sandbox.

Not yet working:

- **The compositor never creates its Wayland socket.** `/run/user/0` contains
  only `bus` and `systemd`, and no `wayland-*` exists anywhere under `/run` or
  `/tmp`. `example/wm.c` calls `wl_display_add_socket_auto()` *before*
  `swc_initialize()`, and on failure returns `EXIT_FAILURE` without printing
  anything — which matches the empty log and the defunct `neoswc-example-wm`
  seen alongside the live one.

  Next step is to confirm what environment the spawned child actually gets:
  `swc-launch` passes its own `environ` through `posix_spawnp` with
  `POSIX_SPAWN_RESETIDS`, and it runs via NixOS's setuid wrapper, so
  `XDG_RUNTIME_DIR` reaching the child is the thing to verify first — dump
  `/proc/<pid>/environ` for the live process rather than inferring it.

## Things that cost time, so they are written down

- **`pkill -f qemu-system-x86_64` kills the calling shell**, because `-f`
  matches the shell's own command line. Use `pkill -f '[q]emu-system-x86_64'`.
- **`-nographic` does not remove QEMU's default VGA.** Without `-vga none` the
  guest gets two DRM devices, and `find_primary_drm_device()` prefers the
  `boot_vga` one — the wrong card. It also makes `screendump` capture the text
  console rather than the compositor.
- **`swc-launch` must be run with `setsid`.** It takes over the VT and changes
  its foreground process group; without a separate session the kernel sends
  `SIGTTOU` to the calling script, which *stops* it. The script goes silent
  without dying, which looks exactly like a hang.
- **Do not run `swc-launch` without `-n` in a headless guest.** `launch.c:493`
  does `VT_ACTIVATE` then `VT_WAITACTIVE`, and with `-vga none` plus
  `console=ttyS0` the VT never becomes active. The wait is uninterruptible, so
  neither `timeout` nor `timeout -s KILL` can recover it.
- **Console output stops once the VT goes graphical**, even though the process
  is still running. That is why diagnostics are mirrored to
  `/tmp/neoswc-vm-share` rather than trusted to the serial console.
- Opening a FIFO read-only blocks until a writer appears, so
  `run-nixos-vm < fifo` never starts qemu. Use `exec 3<>fifo` if driving the
  guest shell interactively.
