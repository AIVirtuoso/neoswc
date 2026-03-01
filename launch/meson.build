launch_protocol = files('protocol.c')
executable('swc-launch',
  ['launch.c', launch_protocol, 'devmajor-@0@.c'.format(os)],
  dependencies: requires_private[0], # libdrm
  install: true,
  install_mode: 'rwsr-xr-x',
)
