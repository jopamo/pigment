# Pigment

<img src="assets/piggy.png" alt="Pigment" width="560">

Pigment combines GdkPixbuf and librsvg in a single C source tree with a
Meson build system for 64-bit Linux and Unix.

It provides raster image decoding, pixel buffers, scaling, compositing,
and SVG rendering through Cairo. Existing applications can continue using
the `gdk-pixbuf-2.0` and `librsvg-2.0` interfaces, including their headers,
library names, and loader APIs.

SVG support is optional. Raster-only builds do not require the SVG
renderer or its dependencies. GTK is not required to build either library.

Pigment's messages are English-only. No translation catalogs are maintained
or installed.

Development focuses on input validation, memory ownership, and rendering
correctness.

## Building

Requirements:

- C compiler
- Meson 0.63 or newer
- Ninja
- pkg-config
- GLib, GObject, and GIO
- shared-mime-info
- libpng and libjpeg

SVG support additionally requires Cairo, Pango, HarfBuzz, Fontconfig,
libxml2, and liblexa.

Meson checks dependency versions. The commands below use system libraries
and disable dependency fallbacks.

Build with SVG support:

```sh
meson setup build --wrap-mode=nofallback
meson compile -C build
```

Build without SVG support:

```sh
meson setup build-raster -Dsvg=false --wrap-mode=nofallback
meson compile -C build-raster
```

GObject Introspection is enabled automatically when available. To control
it explicitly, use `-Dintrospection=enabled` or
`-Dintrospection=disabled`.

See [meson_options.txt](meson_options.txt) for codec and loader options.

Bundled raster loaders support PNG, JPEG, GIF, TIFF, BMP, and ICO/CUR.
BMP and ICO/CUR are disabled by default; enable them with
`-Dbmp=enabled -Dico=enabled`. These options replace `-Dothers=enabled`.
ANI, ICNS, PNM, QTIF, TGA, XBM, and XPM loaders are no longer included.
The public XPM data API remains available for external loader modules.

## Compatibility

Pigment is under active development. API compatibility and rendering
tests are ongoing.

Current limitations:

- SVG and CSS support is incomplete.
- Feature parity with current upstream librsvg is not guaranteed.
- Linux and Unix are targeted, but not all Unix platforms have been tested.
- Windows is not supported.

Pigment installs libraries and headers at the same paths used by GdkPixbuf
and librsvg. **Do not install it over system libraries manually.**
Distribution packages must coordinate replacement to avoid file conflicts.

## Licensing

Pigment incorporates code from GdkPixbuf and librsvg. Original copyright
notices and license terms are retained.

See [COPYING](COPYING), [COPYING.librsvg](COPYING.librsvg), and individual
source files for applicable terms. Libraries, utilities, and artwork may
have different licenses.
