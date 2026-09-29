rsvg_version = '2.52.0'
api_version = '2.0'
api_major = '2'

gnome = import('gnome')
pkgconfig = import('pkgconfig')

cc = meson.get_compiler('c')
rsvg_c_args = cc.get_supported_arguments(['-Wno-c23-extensions', '-Wno-#warnings'])
m_dep = cc.find_library('m', required: false)

glib_dep = dependency('glib-2.0', version: '>=2.12.0')
gobject_dep = dependency('gobject-2.0')
gio_dep = dependency('gio-2.0', version: '>=2.24.0')
gio_unix_dep = dependency('gio-unix-2.0')
gdk_pixbuf_dep = gdkpixbuf_dep
libxml_dep = dependency('libxml-2.0', version: '>=2.9.0')
pangocairo_dep = dependency('pangocairo', version: '>=1.38.0')
pangoft2_dep = dependency('pangoft2', version: '>=1.38.0')
fontconfig_dep = dependency('fontconfig')
cairo_dep = dependency('cairo', version: '>=1.2.0')
cairo_png_dep = dependency('cairo-png', version: '>=1.2.0')

introspection_feature = get_option('introspection')
gi_dep = dependency(
  'gobject-introspection-1.0',
  version: '>=0.10.8',
  required: introspection_feature,
)

bsymbolic_link_args = []
if cc.has_link_argument('-Wl,-Bsymbolic-functions')
  bsymbolic_link_args = ['-Wl,-Bsymbolic-functions']
endif

config_conf = configuration_data()
config_conf.set('HAVE_STRINGS_H', cc.has_header('strings.h'))
config_conf.set('HAVE_STRTOK_R', cc.has_function('strtok_r'))
config_conf.set('HAVE_LC_MESSAGES', cc.has_header_symbol('locale.h', 'LC_MESSAGES'))
config_conf.set('HAVE_PANGOFT2', pangoft2_dep.found())
config_conf.set('HAVE_FLOAT_H', cc.has_header('float.h'))
config_conf.set('HAVE_UNISTD_H', cc.has_header('unistd.h') ? 1 : false)
config_conf.set('PACKAGE', 'librsvg')
config_conf.set('PACKAGE_NAME', 'librsvg')
config_conf.set('PACKAGE_TARNAME', 'librsvg')
config_conf.set('PACKAGE_VERSION', rsvg_version)
config_conf.set('VERSION', rsvg_version)

configure_file(
  input: 'config.h.meson',
  output: 'config.h',
  configuration: config_conf,
)

config_inc = include_directories('.')

css_dep = dependency('liblexa', version: '>=0.1.0', required: true)

src_inc = include_directories('src')
incdir = join_paths(get_option('includedir'), 'librsvg-2.0', 'librsvg')

subdir('src')

if get_option('gdk_pixbuf_loader')
  subdir('gdk-pixbuf-loader')
endif
