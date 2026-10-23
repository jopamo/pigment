/* GdkPixbuf library - Image creation from in-memory buffers
 *
 * Copyright (C) 1999 The Free Software Foundation
 *
 * Author: Federico Mena-Quintero <federico@gimp.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include "gdk-pixbuf.h"
#include "gdk-pixbuf-private.h"
#include <stdlib.h>
#include <string.h>

static gboolean pixel_data_size(int width, int height, int rowstride, guint channels, gsize* size) {
    gsize row_width, preceding_rows;

    return rowstride > 0 && g_size_checked_mul(&row_width, (gsize)width, channels) && (gsize)rowstride >= row_width &&
           g_size_checked_mul(&preceding_rows, (gsize)(height - 1), (gsize)rowstride) &&
           g_size_checked_add(size, preceding_rows, row_width);
}

/**
 * gdk_pixbuf_new_from_data:
 * @data: (array): Image data in 8-bit/sample packed format
 * @colorspace: Colorspace for the image data
 * @has_alpha: Whether the data has an opacity channel
 * @bits_per_sample: Number of bits per sample
 * @width: Width of the image in pixels, must be > 0
 * @height: Height of the image in pixels, must be > 0
 * @rowstride: Distance in bytes between row starts
 * @destroy_fn: (scope async) (allow-none) (closure destroy_fn_data): Function used to free the data when the pixbuf's
 * reference count drops to zero, or %NULL if the data should not be freed
 * @destroy_fn_data: Closure data to pass to the destroy notification function
 *
 * Creates a new #GdkPixbuf out of in-memory image data.
 *
 * Currently only RGB images with 8 bits per sample are supported.
 *
 * Since you are providing a pre-allocated pixel buffer, you must also
 * specify a way to free that data.  This is done with a function of
 * type `GdkPixbufDestroyNotify`.  When a pixbuf created with is
 * finalized, your destroy notification function will be called, and
 * it is its responsibility to free the pixel array.
 *
 * See also: [ctor@GdkPixbuf.Pixbuf.new_from_bytes]
 *
 * Return value: (transfer full): A newly-created pixbuf
 **/
GdkPixbuf* gdk_pixbuf_new_from_data(const guchar* data,
                                    GdkColorspace colorspace,
                                    gboolean has_alpha,
                                    int bits_per_sample,
                                    int width,
                                    int height,
                                    int rowstride,
                                    GdkPixbufDestroyNotify destroy_fn,
                                    gpointer destroy_fn_data) {
    GdkPixbuf* pixbuf;
    gsize required_size;

    /* Only 8-bit/sample RGB buffers are supported for now */

    g_return_val_if_fail(data != NULL, NULL);
    g_return_val_if_fail(colorspace == GDK_COLORSPACE_RGB, NULL);
    g_return_val_if_fail(bits_per_sample == 8, NULL);
    g_return_val_if_fail(width > 0, NULL);
    g_return_val_if_fail(height > 0, NULL);
    g_return_val_if_fail(pixel_data_size(width, height, rowstride, has_alpha ? 4 : 3, &required_size), NULL);

    pixbuf = g_object_new(GDK_TYPE_PIXBUF, "colorspace", colorspace, "n-channels", has_alpha ? 4 : 3, "bits-per-sample",
                          bits_per_sample, "has-alpha", has_alpha ? TRUE : FALSE, "width", width, "height", height,
                          "rowstride", rowstride, "pixels", data, NULL);
    g_assert(pixbuf->storage == STORAGE_PIXELS);
    pixbuf->s.pixels.destroy_fn = destroy_fn;
    pixbuf->s.pixels.destroy_fn_data = destroy_fn_data;

    return pixbuf;
}

/**
 * gdk_pixbuf_new_from_bytes:
 * @data: Image data in 8-bit/sample packed format inside a #GBytes
 * @colorspace: Colorspace for the image data
 * @has_alpha: Whether the data has an opacity channel
 * @bits_per_sample: Number of bits per sample
 * @width: Width of the image in pixels, must be > 0
 * @height: Height of the image in pixels, must be > 0
 * @rowstride: Distance in bytes between row starts
 *
 * Creates a new #GdkPixbuf out of in-memory readonly image data.
 *
 * Currently only RGB images with 8 bits per sample are supported.
 *
 * This is the `GBytes` variant of gdk_pixbuf_new_from_data(), useful
 * for language bindings.
 *
 * Return value: (transfer full): A newly-created pixbuf
 *
 * Since: 2.32
 **/
GdkPixbuf* gdk_pixbuf_new_from_bytes(GBytes* data,
                                     GdkColorspace colorspace,
                                     gboolean has_alpha,
                                     int bits_per_sample,
                                     int width,
                                     int height,
                                     int rowstride) {
    const guint n_channels = has_alpha ? 4u : 3u;
    gsize required_size;

    g_return_val_if_fail(data != NULL, NULL);
    g_return_val_if_fail(colorspace == GDK_COLORSPACE_RGB, NULL);
    g_return_val_if_fail(bits_per_sample == 8, NULL);
    g_return_val_if_fail(width > 0, NULL);
    g_return_val_if_fail(height > 0, NULL);
    g_return_val_if_fail(pixel_data_size(width, height, rowstride, n_channels, &required_size), NULL);
    g_return_val_if_fail(g_bytes_get_size(data) >= required_size, NULL);

    return g_object_new(GDK_TYPE_PIXBUF, "pixel-bytes", data, "colorspace", colorspace, "n-channels", (int)n_channels,
                        "bits-per-sample", bits_per_sample, "has-alpha", has_alpha ? TRUE : FALSE, "width", width,
                        "height", height, "rowstride", rowstride, NULL);
}
