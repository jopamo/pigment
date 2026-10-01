rsvg_version = '2.52.0'
api_version = '2.0'
api_major = '2'

gnome = import('gnome')
pkgconfig = import('pkgconfig')

cc = meson.get_compiler('c')
rsvg_c_args = cc.get_supported_arguments(['-Wno-c23-extensions', '-Wno-#warnings'])
m_dep = cc.find_library('m', required: false)

glib_dep = dependency('glib-2.0', version: '>=2.12.0', allow_fallback: false)
gobject_dep = dependency('gobject-2.0', allow_fallback: false)
gio_dep = dependency('gio-2.0', version: '>=2.24.0', allow_fallback: false)
gio_unix_dep = dependency('gio-unix-2.0', allow_fallback: false)
gdk_pixbuf_dep = gdkpixbuf_dep
libxml_dep = dependency('libxml-2.0', version: '>=2.9.0', allow_fallback: false)
pangocairo_dep = dependency('pangocairo', version: '>=1.38.0', allow_fallback: false)
pangoft2_dep = dependency('pangoft2', version: '>=1.38.0', allow_fallback: false)
fontconfig_dep = dependency('fontconfig', allow_fallback: false)
cairo_dep = dependency('cairo', version: '>=1.2.0', allow_fallback: false)
cairo_png_dep = dependency('cairo-png', version: '>=1.2.0', allow_fallback: false)

introspection_feature = get_option('introspection')
gi_dep = dependency(
  'gobject-introspection-1.0',
  version: '>=0.10.8',
  required: introspection_feature,
  allow_fallback: false,
)

bsymbolic_link_args = []
if cc.has_link_argument('-Wl,-Bsymbolic-functions')
  bsymbolic_link_args = ['-Wl,-Bsymbolic-functions']
endif

config_conf = configuration_data()
rooted_file_io = false
if host_machine.system() == 'linux'
  rooted_file_io = cc.links('''
    #define _GNU_SOURCE 1
    #include <fcntl.h>
    #include <unistd.h>
    int main(void) {
      int fd = openat(AT_FDCWD, "/", O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      return close(fd);
    }
  ''', name: 'directory-relative SVG file reads')
  if not rooted_file_io
    error('Linux SVG file reads require openat and no-follow directory descriptors')
  endif
endif
config_conf.set('HAVE_RSVG_ROOTED_FILE_IO', rooted_file_io)
config_conf.set('HAVE_XML_PARSE_NO_XXE',
  cc.has_header_symbol('libxml/parser.h', 'XML_PARSE_NO_XXE', dependencies: libxml_dep))
config_conf.set('HAVE_STRINGS_H', cc.has_header('strings.h'))
config_conf.set('HAVE_STRTOK_R', cc.has_function('strtok_r'))
config_conf.set('HAVE_LC_MESSAGES', cc.has_header_symbol('locale.h', 'LC_MESSAGES'))

configure_file(
  input: 'config.h.meson',
  output: 'config.h',
  configuration: config_conf,
)

config_inc = include_directories('.')

css_dep = dependency('liblexa', version: '>=0.1.0', required: true, allow_fallback: false)

incdir = join_paths(get_option('includedir'), 'librsvg-2.0', 'librsvg')
