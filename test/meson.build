# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 neoswc contributors
#
# Unit tests for internal libswc modules. These compile the module sources in
# directly rather than linking libswc: the library is built with hidden
# visibility, and these modules are deliberately free of libswc dependencies so
# they can be exercised without a display.

transaction_test = executable('transaction_test',
  ['transaction_test.c'] + swc_transaction_src,
  include_directories: swc_transaction_inc,
  dependencies: [dependency('wayland-server')],
)
test('transaction', transaction_test)

# A river-window-management-v1 client, used to drive the compositor's protocol
# implementation in the test VM. Not a unit test; it needs a running server.
river_wm_client = executable('river-wm-client',
  ['river_wm_client.c'] + protos['river-window-management-v1'] + protos['river-xkb-bindings-v1'],
  dependencies: [dependency('wayland-client')],
  install: true,
)

# Regression test for a compositor crash, so it also needs a running server.
# Destroys an xdg_toplevel that still has a subsurface and commits afterwards,
# which left swc dereferencing the destroyed parent's view.
orphan_subsurface = executable('orphan-subsurface',
  ['orphan_subsurface.c'] + protos['xdg-shell'],
  dependencies: [dependency('wayland-client')],
  install: true,
)

# Abandons the cursor, the two ways a client can: destroying its cursor surface
# and hiding the pointer with a NULL one. Both left the session with no pointer.
cursor_orphan = executable('cursor-orphan',
  ['cursor_orphan.c'] + protos['xdg-shell'],
  dependencies: [dependency('wayland-client')],
  install: true,
)
