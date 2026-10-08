svg_public_inc = include_directories('.')
enum_headers = files('rsvg.h')
svg_cairo_header = files('rsvg-cairo.h')[0]
public_headers = enum_headers + [svg_cairo_header] + files('rsvg-deprecated.h')
svg_features_template = files('librsvg-features.h.in')[0]

version_parts = rsvg_version.split('.')
micro_version = 0
if version_parts.length() > 2
  micro_version = version_parts[2].to_int()
endif
features_conf = configuration_data()
features_conf.set('LIBRSVG_MAJOR_VERSION', version_parts[0].to_int())
features_conf.set('LIBRSVG_MINOR_VERSION', version_parts[1].to_int())
features_conf.set('LIBRSVG_MICRO_VERSION', micro_version)
features_conf.set('PACKAGE_VERSION', rsvg_version)

features_header = configure_file(
  input: svg_features_template,
  output: 'librsvg-features.h',
  configuration: features_conf,
  install_dir: incdir,
)


enum_header = gnome.mkenums(
  'librsvg-enum-types.h',
  sources: enum_headers,
  identifier_prefix: 'Rsvg',
  symbol_prefix: 'rsvg',
  fhead: '''#if !defined (__RSVG_RSVG_H_INSIDE__) && !defined (RSVG_COMPILATION)
#warning "Including <librsvg/librsvg-enum-types.h> directly is deprecated."
#endif

#ifndef __LIBRSVG_ENUM_TYPES_H__
#define __LIBRSVG_ENUM_TYPES_H__

#include <glib-object.h>

G_BEGIN_DECLS
''',
  fprod: '/* enumerations from "@filename@" */',
  vhead: '''GType @enum_name@_get_type (void);
#define RSVG_TYPE_@ENUMSHORT@ (@enum_name@_get_type())
''',
  ftail: '''G_END_DECLS

#endif /* __LIBRSVG_ENUM_TYPES_H__ */
''',
  install_header: true,
  install_dir: incdir,
)

enum_source = gnome.mkenums(
  'librsvg-enum-types.c',
  sources: enum_headers,
  identifier_prefix: 'Rsvg',
  symbol_prefix: 'rsvg',
  fhead: '#include "rsvg.h"',
  fprod: '''
/* enumerations from "@filename@" */''',
  vhead: '''GType
@enum_name@_get_type (void)
{
  static GType etype = 0;
  if (etype == 0) {
    static const G@Type@Value values[] = {''',
  vprod: '      { @VALUENAME@, "@VALUENAME@", "@valuenick@" },',
  vtail: '''      { 0, NULL, NULL }
    };
    etype = g_@type@_register_static ("@EnumName@", values);
  }
  return etype;
}
''',
)

install_headers(public_headers, subdir: 'librsvg-2.0/librsvg')
