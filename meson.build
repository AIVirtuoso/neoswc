project('swc', 'c',
  version: '0.0',
  meson_version: '>=1.7.99',
  license: 'MIT',
  default_options: [
    'c_std=c11',
    'warning_level=1',
    'default_library=both',
  ],
)

cc = meson.get_compiler('c')
os = host_machine.system()
pkg = import('pkgconfig')
wl = import('wayland')

add_project_arguments(cc.get_supported_arguments([
  '-fvisibility=hidden',

  '-Werror=implicit-function-declaration',
  '-Werror=implicit-int',
  '-Werror=pointer-sign',
  '-Werror=pointer-arith',
  '-Wno-missing-braces',
]) + [
  '-D_GNU_SOURCE', # Required for mkostemp
  get_option('buildtype').startswith('debug') ? '-DENABLE_DEBUG=1' : [],
], language: 'c')

requires = [
  dependency('wayland-server', version: '>=1.6.0'),
]
requires_private = [
  dependency('libdrm'),
  dependency('pixman-1'),
  dependency('wayland-protocols'),
  dependency('wld'),
  dependency('xkbcommon'),
]

xwayland = dependency('xwayland', required: get_option('xwayland'))
xwayland_deps = [
  'xcb',
  'xcb-composite',
  'xcb-ewmh',
  'xcb-icccm',
]
if xwayland.found()
  add_project_arguments('-DENABLE_XWAYLAND', language: 'c')
  requires_private += xwayland
  foreach dep : xwayland_deps
    requires_private += dependency(dep, required: xwayland.found())
	endforeach
endif

input_backend = get_option('input')
libinput = dependency('libinput', version: '>=0.4', required: input_backend == 'libinput')

if input_backend == 'auto'
  if os == 'linux'
    input_backend = libinput.found() ? 'libinput' : 'evdev'
  elif os == 'netbsd'
    input_backend = 'wscons'
  else
    error('Failed to determine input backend for ' + os)
  endif
endif  

if input_backend == 'libinput'
  udev = dependency('libudev', required: get_option('udev'))
  if udev.found()
    add_project_arguments('-DENABLE_LIBUDEV', language: 'c')
  endif
  requires_private += [libinput, udev]
elif input_backend == 'evdev'
  add_project_arguments([
    '-DEVDEV_KBD_DEVICE="@0@"'.format(get_option('evdev-keyboard')),
    '-DEVDEV_POINTER_DEVICE="@0@"'.format(get_option('evdev-pointer')),
  ], language: 'c')
endif

subdir('protocol')
subdir('cursor')
subdir('launch')
subdir('libswc')

if get_option('extra')
  subdir('extra')
endif

if get_option('example')
  subdir('example')
endif

subdir('wm')
subdir('test')

summary({
  'Xwayland support': xwayland.found(),
  'Input backend': input_backend,
}, bool_yn: true)
