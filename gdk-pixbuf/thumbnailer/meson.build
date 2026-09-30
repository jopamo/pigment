bin = executable('gdk-pixbuf-thumbnailer',
           thumbnailer_sources,
           c_args: common_cflags + [
             '-DTHUMBNAILER_RETURNS_PIXBUF',
             '-DTHUMBNAILER_USAGE="Thumbnail images"',
           ],
           dependencies: gdk_pixbuf_deps + [ gdkpixbuf_dep ],
           install: true)
meson.override_find_program('gdk-pixbuf-thumbnailer', bin)

gdk_pixbuf_print_mime_types = executable('gdk-pixbuf-print-mime-types',
                                         print_mime_types_source,
                                         c_args: common_cflags,
                                         dependencies: gdk_pixbuf_deps + [ gdkpixbuf_dep ])

custom_target('thumbnailer',
              input: thumbnailer_template,
              output: 'gdk-pixbuf-thumbnailer.thumbnailer',
              command: [
                gen_thumbnailer,
                '--printer', gdk_pixbuf_print_mime_types,
                '--pixdata', gdk_pixbuf_pixdata,
                '--loaders', loaders_cache,
                '--bindir', gdk_pixbuf_bindir,
                '@INPUT@',
                '@OUTPUT@',
              ],
              install: true,
              install_dir: join_paths(gdk_pixbuf_datadir, 'thumbnailers'))
