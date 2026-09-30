gdkpixbuf_headers = files(
  'gdk-pixbuf.h',
  'gdk-pixbuf-animation.h',
  'gdk-pixbuf-autocleanups.h',
  'gdk-pixbuf-core.h',
  'gdk-pixbuf-io.h',
  'gdk-pixbuf-loader.h',
  'gdk-pixbuf-macros.h',
  'gdk-pixbuf-simple-anim.h',
  'gdk-pixbuf-transform.h',
)

gdkpixdata_header = files('gdk-pixdata.h')
gdkpixbuf_features_template = files('gdk-pixbuf-features.h.in')[0]
gdkpixbuf_enum_c_template = files('gdk-pixbuf-enum-types.c.template')[0]
gdkpixbuf_enum_h_template = files('gdk-pixbuf-enum-types.h.template')[0]

gdk_pixbuf_inc = include_directories('.')

gdk_pixbuf_api_path = join_paths(gdk_pixbuf_api_name, 'gdk-pixbuf')

gdkpixbuf_features_conf = configuration_data()
gdkpixbuf_features_conf.set('GDK_PIXBUF_MAJOR', gdk_pixbuf_version_major)
gdkpixbuf_features_conf.set('GDK_PIXBUF_MINOR', gdk_pixbuf_version_minor)
gdkpixbuf_features_conf.set('GDK_PIXBUF_MICRO', gdk_pixbuf_version_micro)
gdkpixbuf_features_conf.set('GDK_PIXBUF_VERSION', gdk_pixbuf_version)
gdkpixbuf_features_conf.set('GDK_PIXBUF_API_VERSION', gdk_pixbuf_api_version)
gdkpixbuf_features_conf.set('LT_CURRENT_MINUS_AGE', current_minus_age)

gdk_pixbuf_features_h = configure_file(input: gdkpixbuf_features_template,
                                       output: 'gdk-pixbuf-features.h',
                                       configuration: gdkpixbuf_features_conf,
                                       install_dir: join_paths(gdk_pixbuf_includedir, gdk_pixbuf_api_path))

install_headers(gdkpixbuf_headers + gdkpixdata_header, subdir: gdk_pixbuf_api_path)

gdkpixbuf_marshals = gnome.genmarshal('gdk-pixbuf-marshal',
                                      sources: 'gdk-pixbuf-marshal.list',
                                      prefix: '_gdk_pixbuf_marshal',
                                      install_header: true,
                                      install_dir: join_paths(gdk_pixbuf_includedir, gdk_pixbuf_api_path))

gdkpixbuf_enums = gnome.mkenums('gdk-pixbuf-enum-types',
                                sources: gdkpixbuf_headers,
                                c_template: gdkpixbuf_enum_c_template,
                                h_template: gdkpixbuf_enum_h_template,
                                install_header: true,
                                install_dir: join_paths(gdk_pixbuf_includedir, gdk_pixbuf_api_path))
gdkpixbuf_enum_h = gdkpixbuf_enums[1]
