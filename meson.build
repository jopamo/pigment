project('pigment', 'c',
  version: '0.1.0',
  license: ['LGPL-2.1-or-later', 'LGPL-2.0-or-later'],
  meson_version: '>= 0.63.0',
  default_options: ['buildtype=debugoptimized', 'c_std=gnu99', 'warning_level=1', 'wrap_mode=nofallback'],
)

subdir('src/internal')
subdir('build-aux/pixbuf')
subdir('include/gdk-pixbuf')
subdir('src/pixbuf')
subdir('tools')
subdir('po')
if get_option('svg')
  subdir('build-aux/svg')
  subdir('include/librsvg')
  subdir('src/svg')
  if get_option('gdk_pixbuf_loader')
    subdir('src/loaders')
  endif
endif

if not meson.is_cross_build()
  meson.add_install_script('build-aux/post-install.py',
    gdk_pixbuf_bindir,
    gdk_pixbuf_libdir,
    gdk_pixbuf_binary_version,
  )
endif

summary('Shared modules', build_modules, section: 'Loaders')
summary('Enabled loaders', enabled_loaders, section: 'Loaders')
summary('Builtin loaders', builtin_loaders, section: 'Loaders')

summary('Debugging', get_option('debug'), section: 'Build')
summary('Optimization', get_option('optimization'), section: 'Build')
summary('GIO MIME sniffing', use_gio_mime, section: 'Build')
summary('MediaLib', use_medialib, section: 'Build')
summary('Introspection', build_gir, section: 'Build')
summary('Relocatable', relocatable, section: 'Build')

summary('prefix', gdk_pixbuf_prefix, section: 'Directories')
summary('libdir', gdk_pixbuf_libdir, section: 'Directories')
summary('datadir', gdk_pixbuf_datadir, section: 'Directories')
summary('libexecdir', gdk_pixbuf_libdir, section: 'Directories')
