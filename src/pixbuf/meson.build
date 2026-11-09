pixops_sources = files('pixops.c')
pixops_inc = include_directories('.')
gdkpixbuf_sources = files(
  'gdk-pixbuf.c',
  'gdk-pixbuf-animation.c',
  'gdk-pixbuf-data.c',
  'gdk-pixbuf-io.c',
  'gdk-pixbuf-loader.c',
  'gdk-pixbuf-scale.c',
  'gdk-pixbuf-simple-anim.c',
  'gdk-pixbuf-scaled-anim.c',
  'gdk-pixbuf-util.c',
)
gdkpixdata_sources = files('gdk-pixdata.c')

# Loaders
#  - sources: additional source files
#  - c_args: additional C compiler arguments
#  - enabled: condition for building

codec_dir = '../codecs'
loaders = {
  'png': {
    'sources': [ codec_dir / 'io-png.c' ],
    'enabled': enabled_loaders.contains('png'),
  },
  'bmp': {
    'sources': [ codec_dir / 'io-bmp.c' ],
    'enabled': enabled_loaders.contains('bmp'),
  },
  'gif': {
    'sources': [ codec_dir / 'io-gif.c', codec_dir / 'io-gif-animation.c', codec_dir / 'lzw.c' ],
    'enabled': enabled_loaders.contains('gif'),
  },
  'ico': {
    'sources': [ codec_dir / 'io-ico.c' ],
    'enabled': enabled_loaders.contains('ico'),
  },
  'jpeg': {
    'sources': [ codec_dir / 'io-jpeg.c' ],
    'enabled': enabled_loaders.contains('jpeg'),
  },
  'tiff': {
    'sources': [ codec_dir / 'io-tiff.c' ],
    'enabled': enabled_loaders.contains('tiff'),
  },
}

gdk_pixbuf_cflags = [
  '-DG_LOG_STRUCTURED=1',
  '-DG_LOG_DOMAIN="GdkPixbuf"',
  '-DGDK_PIXBUF_COMPILATION',
  '-DGDK_PIXBUF_PREFIX="@0@"'.format(gdk_pixbuf_prefix),
  '-DGDK_PIXBUF_LOCALEDIR="@0@"'.format(gdk_pixbuf_localedir),
  '-DGDK_PIXBUF_LIBDIR="@0@"'.format(gdk_pixbuf_libdir),
  '-DGDK_PIXBUF_BINARY_VERSION="@0@"'.format(gdk_pixbuf_binary_version),
  '-DGDK_PIXBUF_ENABLE_BACKEND',
  '-DPIXBUF_LIBDIR="@0@"'.format(gdk_pixbuf_loaderdir),
  '-DBUILT_MODULES_DIR="@0@"'.format(meson.current_build_dir()),
]

# Check if we need to build loaders as built-in functionality
included_loaders_cflags = []
included_loaders_deps = []

foreach name, loader: loaders
  sources = loader.get('sources', [])
  cond = loader.get('enabled', true)
  cflags = loader.get('c_args', [])

  if cond and (builtin_loaders.contains(name) or builtin_all_loaders)
    cflag_for_included_loader = ['-DINCLUDE_@0@'.format(name)]
    included_loaders_cflags += cflag_for_included_loader

    mod = static_library('staticpixbufloader-@0@'.format(name),
                         sources,
                         gdkpixbuf_enum_h,
                         dependencies: loaders_deps + gdk_pixbuf_deps,
                         include_directories: [ root_inc, gdk_pixbuf_inc, internal_inc ],
                         c_args: common_cflags + gdk_pixbuf_cflags + cflags + cflag_for_included_loader)

    included_loaders_deps += declare_dependency(link_with: mod)
  endif
endforeach

# The main gdk-pixbuf shared library
gdkpixbuf = library('gdk_pixbuf-2.0',
                    sources: [
                      gdkpixbuf_sources,
                      pixops_sources,
                      gdkpixbuf_enums,
                      gdkpixbuf_marshals,
                      gdkpixdata_sources,
                    ],
                    soversion: soversion,
                    version: libversion,
		    darwin_versions: darwin_versions,
                    c_args: common_cflags + gdk_pixbuf_cflags + included_loaders_cflags,
                    link_args: common_ldflags,
                    include_directories: [
                      root_inc,
                      gdk_pixbuf_inc,
                      pixops_inc,
                      internal_inc,
                    ],
                    dependencies: [
                      gdk_pixbuf_deps,
                      included_loaders_deps,
                    ],
                    install: true)

gir = find_program('g-ir-scanner', required : get_option('introspection'))
build_gir = gir.found() and (not meson.is_cross_build() or get_option('introspection').enabled())

if build_gir
  dependency('gobject-introspection-1.0', version: '>=0.10.8', allow_fallback: false)
  gir_args = [
    '--quiet',
    '-DGDK_PIXBUF_COMPILATION',
    '-DGDK_PIXBUF_ENABLE_BACKEND',
  ]

  gdkpixbuf_gir = gnome.generate_gir(
    gdkpixbuf,
    sources: [
      gdkpixbuf_sources,
      gdkpixbuf_headers,
      gdkpixbuf_enums,
      gdkpixdata_sources,
      gdk_pixbuf_features_h,
    ],
    namespace: 'GdkPixbuf',
    nsversion: gdk_pixbuf_api_version,
    identifier_prefix: 'Gdk',
    symbol_prefix: 'gdk',
    export_packages: 'gdk-pixbuf-2.0',
    includes: [ 'GModule-2.0', 'Gio-2.0', ],
    header: 'gdk-pixbuf/gdk-pixbuf.h',
    install: true,
    extra_args: gir_args,
  )

  gdkpixdata_gir = gnome.generate_gir(
    gdkpixbuf,
    sources: gdkpixdata_sources + gdkpixdata_header,
    namespace: 'GdkPixdata',
    nsversion: gdk_pixbuf_api_version,
    identifier_prefix: 'Gdk',
    symbol_prefix: 'gdk',
    export_packages: 'gdk-pixbuf-2.0',
    includes: [ gdkpixbuf_gir[0] ],
    header: 'gdk-pixbuf/gdk-pixdata.h',
    install: true,
    extra_args: gir_args,
  )

  built_girs = [ gdkpixbuf_gir, gdkpixdata_gir ]
else
  built_girs = []
endif

gdkpixbuf_dep = declare_dependency(
  version: gdk_pixbuf_version,
  link_with: gdkpixbuf,
  include_directories: root_inc,
  dependencies: gdk_pixbuf_deps,
  sources: [ gdkpixbuf_enum_h, built_girs ],
)
meson.override_dependency('gdk-pixbuf-2.0', gdkpixbuf_dep)

# Now check if we are building loaders as installed shared modules
# We do this here because shared modules depend on libgdk-pixbuf
dynamic_loaders = []
dynamic_loaders_dep = []

foreach name, loader: loaders
  sources = loader.get('sources', [])
  cond = loader.get('enabled', true)
  cflags = loader.get('c_args', [])
  name_suffix = []

  # FIXME: workaround for https://gitlab.gnome.org/GNOME/glib/issues/1413
  if host_system == 'darwin'
    name_suffix = 'so'
  endif

  if cond and not (builtin_loaders.contains(name) or builtin_all_loaders)
    mod = shared_module('pixbufloader-@0@'.format(name),
                        sources,
                        gdkpixbuf_enum_h,
                        dependencies: loaders_deps + gdk_pixbuf_deps + [ gdkpixbuf_dep ],
                        include_directories: [ root_inc, gdk_pixbuf_inc, internal_inc ],
                        c_args: common_cflags + gdk_pixbuf_cflags + cflags,
                        name_suffix: name_suffix,
                        install: true,
                        install_dir: gdk_pixbuf_loaderdir)

    # Query this build's selected modules for cache and thumbnailer generation.
    dynamic_loaders += mod.full_path()
    dynamic_loaders_dep += mod
  endif
endforeach

pkgconfig = import('pkgconfig')
pkgconfig.generate(
  gdkpixbuf,
  version: gdk_pixbuf_version,
  name: 'GdkPixbuf',
  description: 'Image loading and scaling',
  variables: [
    'bindir=${prefix}/@0@'.format(get_option('bindir')),
    'gdk_pixbuf_binary_version=@0@'.format(gdk_pixbuf_binary_version),
    'gdk_pixbuf_binarydir=${libdir}/@0@/@1@'.format(gdk_pixbuf_api_name, gdk_pixbuf_binary_version),
    'gdk_pixbuf_moduledir=${gdk_pixbuf_binarydir}/loaders',
    'gdk_pixbuf_cache_file=${gdk_pixbuf_binarydir}/loaders.cache',
    'gdk_pixbuf_csource=${bindir}/gdk-pixbuf-csource',
    'gdk_pixbuf_pixdata=${bindir}/gdk-pixbuf-pixdata',
    'gdk_pixbuf_query_loaders=${bindir}/gdk-pixbuf-query-loaders',
  ],
  requires: 'gobject-2.0',
  subdirs: gdk_pixbuf_api_name,
  filebase: gdk_pixbuf_api_name,
)
