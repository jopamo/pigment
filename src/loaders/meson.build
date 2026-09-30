loader = shared_module(
  'pixbufloader-svg',
  'io-svg.c',
  include_directories: [src_inc, config_inc],
  c_args: ['-DGDK_PIXBUF_ENABLE_BACKEND'],
  dependencies: [
    librsvg_dep,
    gdk_pixbuf_dep,
    glib_dep,
  ],
  name_prefix: 'lib',
  install: true,
  install_dir: gdk_pixbuf_loaderdir,
)

thumbnailer_conf = configuration_data()
thumbnailer_conf.set('bindir', join_paths(get_option('prefix'), get_option('bindir')))

configure_file(
  input: 'librsvg.thumbnailer.in',
  output: 'librsvg.thumbnailer',
  configuration: thumbnailer_conf,
  install_dir: join_paths(get_option('datadir'), 'thumbnailers'),
)
