# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 neoswc contributors

neoswc = executable('neoswc',
  ['main.c', 'river_wm.c'] + protos['river-window-management-v1'] + protos['river-layer-shell-v1'],
  dependencies: libswc,
  include_directories: include_directories('.'),
  install: true,
)
