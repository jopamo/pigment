add_project_arguments([ '-D_POSIX_C_SOURCE=200809L', '-D_DEFAULT_SOURCE', '-D_XOPEN_SOURCE=700' ], language: 'c')

cc = meson.get_compiler('c')
host_system = host_machine.system()

# Versioning
gdk_pixbuf_version = '2.42.13'
version_arr = gdk_pixbuf_version.split('.')
gdk_pixbuf_version_major = version_arr[0].to_int()
gdk_pixbuf_version_minor = version_arr[1].to_int()
gdk_pixbuf_version_micro = version_arr[2].to_int()

gdk_pixbuf_api_version = '2.0'
gdk_pixbuf_binary_version = '2.10.0'

gdk_pixbuf_api_name = 'gdk-pixbuf-@0@'.format(gdk_pixbuf_api_version)

if gdk_pixbuf_version_minor.is_odd()
  gdk_pixbuf_interface_age = 0
else
  gdk_pixbuf_interface_age = gdk_pixbuf_version_micro
endif

gdk_pixbuf_binary_age = 100 * gdk_pixbuf_version_minor + gdk_pixbuf_version_micro

# maintaining compatibility with the previous libtool versioning
# current = binary - interface
# revision = interface
soversion = 0
current = gdk_pixbuf_binary_age - gdk_pixbuf_interface_age
revision = gdk_pixbuf_interface_age
libversion = '@0@.@1@.@2@'.format(soversion, current, revision)
age = gdk_pixbuf_binary_age - gdk_pixbuf_interface_age
current_minus_age = current - age
darwin_versions = ['@0@'.format(current + 1), '@0@.@1@'.format(current + 1, gdk_pixbuf_interface_age)]

# Paths
gdk_pixbuf_prefix = get_option('prefix')
gdk_pixbuf_libdir = join_paths(gdk_pixbuf_prefix, get_option('libdir'))
gdk_pixbuf_bindir = join_paths(gdk_pixbuf_prefix, get_option('bindir'))
gdk_pixbuf_includedir = join_paths(gdk_pixbuf_prefix, get_option('includedir'))
gdk_pixbuf_datadir = join_paths(gdk_pixbuf_prefix, get_option('datadir'))
gdk_pixbuf_mandir = join_paths(gdk_pixbuf_prefix, get_option('mandir'))
gdk_pixbuf_libexecdir = join_paths(gdk_pixbuf_prefix, get_option('libexecdir'))
gdk_pixbuf_loaderdir = join_paths(gdk_pixbuf_libdir, 'gdk-pixbuf-@0@/@1@/loaders'.format(gdk_pixbuf_api_version, gdk_pixbuf_binary_version))

# Dependencies
glib_req_version = '>= 2.56.0'
glib_dep = dependency('glib-2.0', version: glib_req_version,
                      allow_fallback: false)
gobject_dep = dependency('gobject-2.0', version: glib_req_version,
                         allow_fallback: false)
gmodule_dep = dependency('gmodule-no-export-2.0', version: glib_req_version,
                         method: 'pkg-config', allow_fallback: false)
gio_dep = dependency('gio-2.0', version: glib_req_version,
                     allow_fallback: false)

# Configurations
gdk_pixbuf_conf = configuration_data()

mathlib_dep = cc.find_library('m', required: false)

# Common compiler and linker flags
common_cflags = []
common_ldflags = []

if cc.get_id() == 'gcc' or cc.get_id() == 'clang'
  test_cflags = [
    '-Wpointer-arith',
    '-Wformat=2',
    '-Wstrict-prototypes',
    '-Wnested-externs',
    '-Wold-style-definition',
    '-Wdeclaration-after-statement',
    '-Wunused',
    '-Wcast-align',
    '-Wmissing-noreturn',
    '-Wmissing-format-attribute',
    '-Wlogical-op',
    '-fno-strict-aliasing',
    '-Wno-int-conversion',
    '-Wno-uninitialized',
    '-Wno-discarded-qualifiers',
    '-Werror=implicit',
    '-Werror=nonnull',
    '-Werror=init-self',
    '-Werror=main',
    '-Werror=missing-braces',
    '-Werror=sequence-point',
    '-Werror=return-type',
    '-Werror=trigraphs',
    '-Werror=array-bounds',
    '-Werror=write-strings',
    '-Werror=address',
    '-Werror=int-to-pointer-cast',
    '-Werror=pointer-to-int-cast',
    '-Werror=empty-body',
  ]

else
  test_cflags = []
endif

# Symbol visibility
if get_option('default_library') != 'static'
  gdk_pixbuf_conf.set('_GDK_PIXBUF_EXTERN', '__attribute__((visibility("default"))) extern')
  test_cflags += ['-fvisibility=hidden']
endif

common_cflags += cc.get_supported_arguments(test_cflags)

if host_machine.system() == 'linux'
  # Additional linker flags
  test_ldflags = ['-Wl,-Bsymbolic', '-Wl,-z,relro', '-Wl,-z,now']
  common_ldflags += cc.get_supported_link_arguments(test_ldflags)
  gdk_pixbuf_conf.set('OS_LINUX', 1)
endif

if host_machine.system() == 'darwin'
  gdk_pixbuf_conf.set('OS_DARWIN', 1)
endif

# On non-macOS/Android systems we require shared-mime-info and GIO
# shared_mime_dep = []
shared_mime_dep = []
use_gio_mime = false
if get_option('gio_sniffing') and host_system not in ['darwin', 'android']
  shared_mime_dep += dependency('shared-mime-info', allow_fallback: false)
  gdk_pixbuf_conf.set('GDK_PIXBUF_USE_GIO_MIME', 1)
  use_gio_mime = true
endif

gdk_pixbuf_deps = [
  mathlib_dep,
  glib_dep,
  gobject_dep,
  gmodule_dep,
  gio_dep,
  shared_mime_dep,
]

# Check if we can build shared modules
build_modules = gmodule_dep.get_variable(pkgconfig: 'gmodule_supported') == 'true'
gdk_pixbuf_conf.set('USE_GMODULE', build_modules)

# Check which loaders should be built into gdk-pixbuf
builtin_loaders = get_option('builtin_loaders')

# If 'all' is specified for builtin_loaders, build all
# buildable loaders into gdk-pixbuf
builtin_all_loaders = false
if builtin_loaders.contains('all')
  builtin_all_loaders = true
elif builtin_loaders.contains('none')
  builtin_loaders = []
endif

# Loader dependencies
enabled_loaders = []
loaders_deps = []

png_opt = get_option('png')
if not png_opt.disabled()
  png_dep = dependency('libpng',
    required: png_opt, allow_fallback: false)

  if png_dep.found()
    enabled_loaders += 'png'
    loaders_deps += png_dep
  endif
endif

gif_opt = get_option('gif')
if not gif_opt.disabled()
  enabled_loaders += 'gif'
endif

foreach loader: ['bmp', 'ico']
  if not get_option(loader).disabled()
    enabled_loaders += loader
  endif
endforeach

jpeg_opt = get_option('jpeg')
if not jpeg_opt.disabled()
  jpeg_dep = dependency('libjpeg',
    required: jpeg_opt, allow_fallback: false)

  if jpeg_dep.found()
    enabled_loaders += 'jpeg'
    loaders_deps += jpeg_dep
  endif
endif

tiff_opt = get_option('tiff')
if not tiff_opt.disabled()
  tiff_dep = dependency('libtiff-4',
    required: tiff_opt, allow_fallback: false)

  if tiff_dep.found()
    enabled_loaders += 'tiff'
    loaders_deps += tiff_dep
  endif
endif

relocatable = get_option('relocatable')

if relocatable
  add_project_arguments([ '-DGDK_PIXBUF_RELOCATABLE' ], language: 'c')
endif

configure_file(output: 'config.h', configuration: gdk_pixbuf_conf)
add_project_arguments([ '-DHAVE_CONFIG_H=1' ], language: 'c')

root_inc = include_directories('.', '../../include')

# Auxiliary scripts
gen_thumbnailer = find_program('../gen-thumbnailer.py')
gen_loader_cache = find_program('../gen-loader-cache.py')

gnome = import('gnome')
