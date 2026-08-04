fs = import('fs')

# Allow easy lookup for protocol for utility applications
protos = {}
foreach proto : [
  'server-decoration.xml',
  'swc.xml',
  'swc_snap.xml',
  'swc_select.xml',
  'wlr-layer-shell-unstable-v1.xml',
  # Screen capture. wayland-protocols ships these in staging; vendored so the
  # build does not depend on the host's wayland-protocols version having them.
  # Only for the interface symbols: ext-image-capture-source references
  # ext_foreign_toplevel_handle_v1 in its per-toplevel source manager, so the
  # generated table will not link without it. No global is created for it --
  # per-window capture is not implemented, only whole-output.
  'ext-foreign-toplevel-list-v1.xml',
  'ext-image-capture-source-v1.xml',
  'ext-image-copy-capture-v1.xml',
  # The clipboard protocol for programs that are not the focused window:
  # clipboard managers, wl-paste, and every clipboard library that wraps them.
  # wl_data_device only ever hands the selection to the client with keyboard
  # focus, which such a program never has. Vendored for the same reason as the
  # capture protocols -- it is staging, so the host's wayland-protocols may be
  # too old to have it.
  'ext-data-control-v1.xml',
  # Vendored from wlr-protocols (MIT-style, Purism SPC). The only protocol
  # kanshi and wlr-randr speak; wayland-protocols has no output-management
  # equivalent, staging or otherwise. Our implementation of it is GPL.
  'wlr-output-management-unstable-v1.xml',
  'wayland-drm.xml',
  # Vendored from river (MIT); the implementation of it is ours and GPL.
  'river-window-management-v1.xml',
  'river-layer-shell-v1.xml',
  'river-xkb-bindings-v1.xml',
  wl.find_protocol('xdg-shell', state: 'stable'),
  wl.find_protocol('linux-dmabuf', state: 'unstable', version: 1),
  wl.find_protocol('xdg-decoration', state: 'unstable', version: 1),
  wl.find_protocol('xdg-output', state: 'unstable', version: 1),
]
  protocols = wl.scan_xml(proto,
    public: true,
    client: true,
    server: true)
  protos += {fs.stem(fs.name(proto)): protocols}
endforeach
install_data('swc.xml', install_dir: get_option('datadir') / 'swc')
