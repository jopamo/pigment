svg_inc = include_directories('.')
lib_sources = files(
  'librsvg-features.c',
  'rsvg-base-file-util.c',
  'rsvg-base.c',
  'rsvg-cairo-clip.c',
  'rsvg-cairo-draw.c',
  'rsvg-cairo-render.c',
  'rsvg-cond.c',
  'rsvg-css-engine.c',
  'rsvg-css-engine-native.c',
  'rsvg-css.c',
  'rsvg-defs.c',
  'rsvg-file-util.c',
  'rsvg-filter.c',
  'rsvg-gobject.c',
  'rsvg-image.c',
  'rsvg-io.c',
  'rsvg-marker.c',
  'rsvg-mask.c',
  'rsvg-paint-server.c',
  'rsvg-path.c',
  'rsvg-shapes.c',
  'rsvg-size-callback.c',
  'rsvg-structure.c',
  'rsvg-styles.c',
  'rsvg-text.c',
  'rsvg-xml.c',
  'rsvg.c',
)


lib_deps = [
  glib_dep,
  gobject_dep,
  gio_dep,
  gdk_pixbuf_dep,
  libxml_dep,
  pangocairo_dep,
  pangoft2_dep,
  harfbuzz_dep,
  cairo_dep,
  cairo_png_dep,
  css_dep,
  fontconfig_dep,
]
if rooted_file_io
  lib_deps += gio_unix_dep.partial_dependency(compile_args: true, includes: true)
endif

if m_dep.found()
  lib_deps += m_dep
endif

lib_includes = [
  config_inc,
  svg_public_inc,
  svg_inc,
]

lib_c_args = rsvg_c_args + [
  '-DG_LOG_DOMAIN="librsvg"',
  '-DRSVG_DISABLE_DEPRECATION_WARNINGS',
  '-DRSVG_COMPILATION',
]

librsvg_lib = both_libraries(
  'rsvg-' + api_major,
  sources: lib_sources + [enum_source, enum_header],
  override_options: ['c_std=c99'],
  c_args: lib_c_args,
  include_directories: lib_includes,
  dependencies: lib_deps,
  version: rsvg_version,
  soversion: api_major,
  link_args: bsymbolic_link_args,
  install: true,
)

librsvg_dep = declare_dependency(
  link_with: librsvg_lib.get_shared_lib(),
  include_directories: lib_includes,
  dependencies: lib_deps,
  sources: [enum_header, features_header],
)

pkgconfig.generate(
  libraries: librsvg_lib,
  version: rsvg_version,
  name: 'librsvg-2.0',
  filebase: 'librsvg-2.0',
  description: 'Library to render SVG files',
  subdirs: 'librsvg-2.0',
  requires: [
    'gio-2.0',
    'gdk-pixbuf-2.0',
    'glib-2.0',
    'gobject-2.0',
    'libxml-2.0',
    'pangoft2',
    'pangocairo',
    'cairo',
    'cairo-png',
    'fontconfig',
  ],
  requires_private: ['harfbuzz >=2.6.0'],
)

if build_gir
  gir_extra_args = [
    '--c-include=librsvg/rsvg.h',
    '--symbol-prefix=rsvg',
    '--identifier-prefix=Rsvg',
    '--warn-all',
    '-DRSVG_COMPILATION',
  ]

  gnome.generate_gir(
    librsvg_lib,
    sources: lib_sources + enum_headers + [svg_cairo_header, enum_source],
    nsversion: api_version,
    namespace: 'Rsvg',
    include_directories: lib_includes,
    includes: [
      'GObject-2.0',
      'Gio-2.0',
      'cairo-1.0',
      gdkpixbuf_gir[0],
    ],
    export_packages: 'librsvg-2.0',
    header: 'librsvg/rsvg.h',
    install: true,
    install_dir_gir: join_paths(get_option('datadir'), 'gir-1.0'),
    install_dir_typelib: join_paths(get_option('libdir'), 'girepository-1.0'),
    extra_args: gir_extra_args,
    dependencies: lib_deps,
  )
endif

meson.override_dependency('librsvg-2.0', librsvg_dep)
