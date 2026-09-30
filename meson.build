project('pigment', 'c',
  version: '0.1.0',
  license: ['LGPL-2.1-or-later', 'LGPL-2.0-or-later'],
  meson_version: '>= 0.63.0',
  default_options: ['buildtype=debugoptimized', 'c_std=gnu99', 'warning_level=1', 'wrap_mode=nofallback'],
)

subdir('src/internal')
subdir('src/pixbuf')
subdir('include/gdk-pixbuf')
subdir('tools')
subdir('gdk-pixbuf')
subdir('po')
if get_option('svg')
  subdir('include/librsvg')
  subdir('src/svg')
  subdir('librsvg')
  if get_option('gdk_pixbuf_loader')
    subdir('src/loaders')
  endif
endif
