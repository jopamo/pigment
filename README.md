<img src="assets/piggy.png" alt="Pigment" width="560">

Pigment brings GdkPixbuf and librsvg into one C source tree for Linux and Unix. It provides raster image loading, pixel buffers, scaling, compositing, and Cairo-based SVG rendering.

It uses the existing `gdk-pixbuf-2.0` and `librsvg-2.0` library names, headers, and pkg-config interfaces. SVG support is optional, and neither library requires GTK to build.

## Building

You need a C compiler, Meson 0.63+, Ninja, pkg-config, GLib 2.56+ (including GObject, GIO, and GModule), libpng, and libjpeg. The default Linux build also requires shared-mime-info.

SVG builds additionally require Cairo, Pango, HarfBuzz, Fontconfig, libxml2, gio-unix-2.0, and liblexa 0.1+.

Build with SVG support (the default):

```sh
meson setup build
meson compile -C build
```

For a raster-only build:

```sh
meson setup build-raster -Dsvg=false
meson compile -C build-raster
```

Dependency fallbacks are disabled by default. GObject Introspection is built when available; use `-Dintrospection=enabled` or `-Dintrospection=disabled` to override this.

Pigment includes PNG, JPEG, GIF, TIFF, BMP, and ICO/CUR loaders. PNG, JPEG, and GIF are enabled by default; TIFF is enabled when libtiff is available. BMP and ICO/CUR are disabled by default and can be enabled with `-Dbmp=enabled -Dico=enabled`. See [meson_options.txt](meson_options.txt) for the remaining options.

## Compatibility

Pigment is still in development. SVG and CSS support is incomplete, and full compatibility with upstream GdkPixbuf and librsvg has not been established. Windows is not supported.

ANI, ICNS, PNM, QTIF, TGA, XBM, and XPM loaders are not included. The XPM data API remains available but requires an external loader module. Diagnostic messages are English-only; no translations are shipped.

**Do not install Pigment over system libraries manually.** Its library and header paths overlap with GdkPixbuf and librsvg; distribution packages must handle replacement and file conflicts.

## Licensing

Pigment contains code from GdkPixbuf and librsvg. See [COPYING](COPYING), [COPYING.librsvg](COPYING.librsvg), and the source files for applicable licenses. Tools and artwork may have different terms.
