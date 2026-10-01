gdkpixbuf_bin = [
  [ 'gdk-pixbuf-csource', files('gdk-pixbuf-csource.c') ],
  [ 'gdk-pixbuf-pixdata', files('gdk-pixbuf-pixdata.c') ],
  [ 'gdk-pixbuf-query-loaders', files('queryloaders.c') ],
]
thumbnailer_sources = files('gdk-pixbuf-thumbnailer.c', 'gnome-thumbnailer-skeleton.c')
print_mime_types_source = files('gdk-pixbuf-print-mime-types.c')
thumbnailer_template = files('gdk-pixbuf-thumbnailer.thumbnailer.in')[0]

foreach bin: gdkpixbuf_bin
  bin_name = bin[0]
  bin_source = bin[1]

  bin = executable(bin_name, bin_source,
                   gdkpixbuf_enum_h,
                   dependencies: gdk_pixbuf_deps + [ gdkpixbuf_dep ],
                   include_directories: [ root_inc, gdk_pixbuf_inc, pixops_inc ],
                   c_args: common_cflags + gdk_pixbuf_cflags,
                   install: true)
  # Expose tools to cache and thumbnailer generation.
  set_variable(bin_name.underscorify(), bin)
endforeach

if not meson.is_cross_build()
  # Generate the cache from this build's selected dynamic modules.
  loaders_cache = custom_target('loaders.cache',
                                output: 'loaders.cache',
                                capture: true,
                                command: [
                                  gen_loader_cache,
                                  gdk_pixbuf_query_loaders,
                                  dynamic_loaders,
                                ],
                                depends: dynamic_loaders_dep,
                                build_by_default: true)
else
  loaders_cache = []
endif

if not meson.is_cross_build()
  bin = executable('gdk-pixbuf-thumbnailer',
             thumbnailer_sources,
             c_args: common_cflags + [
               '-DTHUMBNAILER_RETURNS_PIXBUF',
               '-DTHUMBNAILER_USAGE="Thumbnail images"',
             ],
             dependencies: gdk_pixbuf_deps + [ gdkpixbuf_dep ],
             install: true)

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
endif
