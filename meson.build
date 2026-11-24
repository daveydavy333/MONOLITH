project('monolith', 'c',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>=0.60.0',
  default_options: ['c_std=c11', 'warning_level=2', 'buildtype=debugoptimized'])

fs = import('fs')

add_project_arguments('-DWLR_USE_UNSTABLE', language: 'c')
if get_option('testing')
  add_project_arguments('-DMONOLITH_TESTING', language: 'c')
endif

wayland_server = dependency('wayland-server')
wlroots        = dependency(['wlroots-0.18', 'wlroots-0.19', 'wlroots-0.20'])
xkbcommon      = dependency('xkbcommon')
pixman         = dependency('pixman-1')
libdrm         = dependency('libdrm')
libinput       = dependency('libinput')
libm           = meson.get_compiler('c').find_library('m', required: false)

# --- generated protocol headers (server side) --------------------------
wayland_scanner = find_program('wayland-scanner', native: true)
wl_protos = dependency('wayland-protocols').get_variable('pkgdatadir')

protocol_xmls = [
  wl_protos / 'stable/xdg-shell/xdg-shell.xml',
  wl_protos / 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml',
  wl_protos / 'staging/cursor-shape/cursor-shape-v1.xml',
  wl_protos / 'stable/tablet/tablet-v2.xml',
  wl_protos / 'unstable/xdg-foreign/xdg-foreign-unstable-v1.xml',
  wl_protos / 'unstable/xdg-foreign/xdg-foreign-unstable-v2.xml',
  'protocols/wlr-layer-shell-unstable-v1.xml',
]

proto_headers = []
foreach xml : protocol_xmls
  proto_headers += custom_target(
    'hdr-' + fs.name(xml),
    input: xml,
    output: '@BASENAME@-protocol.h',
    command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
endforeach

# --- vendored tomlc17 + config + compositor ----------------------------
monolith_exe = executable('monolith',
  ['src/monolith.c', 'src/config.c', 'src/tomlc17.c', proto_headers],
  dependencies: [wayland_server, wlroots, xkbcommon, pixman, libdrm, libinput, libm],
  install: true)

executable('monolithctl', 'src/monolithctl.c', install: true)

install_data('data/config.toml', install_dir: get_option('datadir') / 'monolith')
install_data('data/monolith-portals.conf', install_dir: get_option('datadir') / 'xdg-desktop-portal')
