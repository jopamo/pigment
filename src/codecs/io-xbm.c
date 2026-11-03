/* -*- mode: C; c-file-style: "linux" -*- */
/* GdkPixbuf library - XBM image loader
 *
 * Copyright (C) 1999 Mark Crichton
 * Copyright (C) 1999 The Free Software Foundation
 * Copyright (C) 2001 Eazel, Inc.
 *
 * Authors: Mark Crichton <crichton@gimp.org>
 *          Federico Mena-Quintero <federico@gimp.org>
 *          Jonathan Blandford <jrb@redhat.com>
 *	    John Harper <jsh@eazel.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

/* Following code adapted from io-tiff.c, which was ``(almost) blatantly
   ripped from Imlib'' */

#include "config.h"
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <stdio.h>
#include <errno.h>
#include <glib/gstdio.h>
#include <glib/gi18n-lib.h>

#include "gdk-pixbuf-io.h"

typedef struct _XBMData XBMData;
struct _XBMData {
    GdkPixbufModulePreparedFunc prepared_func;
    GdkPixbufModuleUpdatedFunc updated_func;
    gpointer user_data;

    gchar* tempname;
    FILE* file;
    gboolean all_okay;
};

/* xbm parser borrowed from xc/lib/X11/RdBitF.c */

#define MAX_SIZE 255

/* shared data for the image read/parse logic */
static short hex_table[256];        /* conversion value */
static gsize hex_table_initialized; /* easier to fill in at run time */

/* Table index for the hex values. Initialized once, first time.
 * Used for translation value or delimiter significance lookup.
 */
static void init_hex_table(void) {
    /*
     * We build the table at run time for several reasons:
     *
     * 1. portable to non-ASCII machines.
     * 2. still reentrant since we set the init flag after setting table.
     * 3. easier to extend.
     * 4. less prone to bugs.
     */
    hex_table['0'] = 0;
    hex_table['1'] = 1;
    hex_table['2'] = 2;
    hex_table['3'] = 3;
    hex_table['4'] = 4;
    hex_table['5'] = 5;
    hex_table['6'] = 6;
    hex_table['7'] = 7;
    hex_table['8'] = 8;
    hex_table['9'] = 9;
    hex_table['A'] = 10;
    hex_table['B'] = 11;
    hex_table['C'] = 12;
    hex_table['D'] = 13;
    hex_table['E'] = 14;
    hex_table['F'] = 15;
    hex_table['a'] = 10;
    hex_table['b'] = 11;
    hex_table['c'] = 12;
    hex_table['d'] = 13;
    hex_table['e'] = 14;
    hex_table['f'] = 15;

    /* delimiters of significance are flagged w/ negative value */
    hex_table[' '] = -1;
    hex_table[','] = -1;
    hex_table['}'] = -1;
    hex_table['\n'] = -1;
    hex_table['\t'] = -1;
}

/* Read next hex value in the input stream, return -1 if EOF */
static int next_int(FILE* fstream) {
    int ch;
    int value = 0;
    int gotone = 0;
    int done = 0;

    /* loop, accumulate hex value until find delimiter
       skip any initial delimiters found in read stream */

    while (!done) {
        ch = getc(fstream);
        if (ch == EOF) {
            value = -1;
            done++;
        }
        else {
            /* trim high bits, check type and accumulate */
            ch &= 0xff;
            if (g_ascii_isxdigit(ch)) {
                value = ((value & 0xf) << 4) + g_ascii_xdigit_value(ch);
                gotone = 1;
            }
            else if ((hex_table[ch]) < 0 && gotone) {
                done++;
            }
        }
    }
    return value;
}

static const gchar* xbm_skip_spaces(const gchar* p) {
    while (*p != '\0' && g_ascii_isspace(*p))
        p++;
    return p;
}

static gboolean xbm_parse_define_line(const gchar* line,
                                      gchar* name_and_type,
                                      gsize name_and_type_size,
                                      gint* value_out) {
    const gchar* p = line;
    const gchar* start;
    gchar* endptr = NULL;
    gint64 v;
    gsize len;

    p = xbm_skip_spaces(p);
    if (*p != '#')
        return FALSE;

    p++;
    p = xbm_skip_spaces(p);
    if (!g_str_has_prefix(p, "define"))
        return FALSE;

    p += strlen("define");
    if (*p != '\0' && !g_ascii_isspace(*p))
        return FALSE;

    p = xbm_skip_spaces(p);
    start = p;
    while (*p != '\0' && !g_ascii_isspace(*p))
        p++;

    len = (gsize)(p - start);
    if (len == 0 || len >= name_and_type_size)
        return FALSE;

    memcpy(name_and_type, start, len);
    name_and_type[len] = '\0';

    p = xbm_skip_spaces(p);
    if (*p == '\0')
        return FALSE;

    errno = 0;
    v = g_ascii_strtoll(p, &endptr, 10);
    if (endptr == p)
        return FALSE;

    if (errno == ERANGE || v < G_MININT || v > G_MAXINT)
        return FALSE;

    *value_out = (gint)v;
    return TRUE;
}

static gboolean xbm_parse_static_bits_decl_line(const gchar* line,
                                                gchar* name_and_type,
                                                gsize name_and_type_size,
                                                gint* version10p_out) {
    const gchar* p = line;
    const gchar* start;
    gsize len;

    p = xbm_skip_spaces(p);

    if (g_str_has_prefix(p, "static short ")) {
        *version10p_out = 1;
        p += strlen("static short ");
    }
    else if (g_str_has_prefix(p, "static const unsigned char ")) {
        *version10p_out = 0;
        p += strlen("static const unsigned char ");
    }
    else if (g_str_has_prefix(p, "static unsigned char ")) {
        *version10p_out = 0;
        p += strlen("static unsigned char ");
    }
    else if (g_str_has_prefix(p, "static const char ")) {
        *version10p_out = 0;
        p += strlen("static const char ");
    }
    else if (g_str_has_prefix(p, "static char ")) {
        *version10p_out = 0;
        p += strlen("static char ");
    }
    else {
        return FALSE;
    }

    p = xbm_skip_spaces(p);
    start = p;
    while (*p != '\0' && !g_ascii_isspace(*p))
        p++;

    len = (gsize)(p - start);
    if (len == 0 || len >= name_and_type_size)
        return FALSE;

    memcpy(name_and_type, start, len);
    name_and_type[len] = '\0';

    p = xbm_skip_spaces(p);
    if (*p != '=')
        return FALSE;

    p++;
    p = xbm_skip_spaces(p);
    if (*p != '{')
        return FALSE;

    return TRUE;
}

static gboolean read_bitmap_file_data(FILE* fstream,
                                      guint* width,
                                      guint* height,
                                      guchar** data,
                                      int* x_hot,
                                      int* y_hot,
                                      GError** error) {
    guchar* bits = NULL;          /* working variable */
    char line[MAX_SIZE];          /* input line from file */
    guint size;                   /* number of bytes of data */
    char name_and_type[MAX_SIZE]; /* an input line */
    int value;                    /* from an input line */
    int version10p;               /* boolean, old format */
    int padding;                  /* to handle alignment */
    int bytes_per_line;           /* per scanline of data */
    guint ww = 0;                 /* width */
    guint hh = 0;                 /* height */
    int hx = -1;                  /* x hotspot */
    int hy = -1;                  /* y hotspot */

    /* first time initialization */
    if (g_once_init_enter(&hex_table_initialized)) {
        init_hex_table();
        g_once_init_leave(&hex_table_initialized, 1);
    }

    /* error cleanup and return macro */
#define RETURN(code)  \
    {                 \
        g_free(bits); \
        return code;  \
    }

    while (fgets(line, MAX_SIZE, fstream)) {
        if (strlen(line) == MAX_SIZE - 1)
            RETURN(FALSE);
        if (xbm_parse_define_line(line, name_and_type, sizeof(name_and_type), &value)) {
            if ((strcmp(name_and_type, "width") == 0) || g_str_has_suffix(name_and_type, "_width")) {
                if (value <= 0 || value > G_MAXINT)
                    RETURN(FALSE);
                ww = (guint)value;
            }
            else if ((strcmp(name_and_type, "height") == 0) || g_str_has_suffix(name_and_type, "_height")) {
                if (value <= 0 || value > G_MAXINT)
                    RETURN(FALSE);
                hh = (guint)value;
            }
            else if ((strcmp(name_and_type, "x_hot") == 0) || g_str_has_suffix(name_and_type, "_x_hot")) {
                hx = value;
            }
            else if ((strcmp(name_and_type, "y_hot") == 0) || g_str_has_suffix(name_and_type, "_y_hot")) {
                hy = value;
            }
            continue;
        }

        if (!xbm_parse_static_bits_decl_line(line, name_and_type, sizeof(name_and_type), &version10p))
            continue;

        if (strcmp(name_and_type, "bits[]") != 0 && !g_str_has_suffix(name_and_type, "_bits[]"))
            continue;

        if (!ww || !hh)
            RETURN(FALSE);

        /* Choose @padding so @size is even if @version10p is %TRUE.
         * If @version10p is %FALSE, @size could be even or odd. */
        if ((ww % 16) && ((ww % 16) < 9) && version10p)
            padding = 1;
        else
            padding = 0;

        /* Check for overflow for the bytes_per_line calculation. */
        if (ww > G_MAXUINT - 7)
            RETURN(FALSE);

        bytes_per_line = (ww + 7) / 8 + padding;
        g_assert(!version10p || (bytes_per_line % 2) == 0);

        /* size = bytes_per_line * hh */
        if (!g_uint_checked_mul(&size, bytes_per_line, hh))
            RETURN(FALSE);

        bits = g_try_malloc(size);
        if (bits == NULL) {
            g_set_error_literal(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_INSUFFICIENT_MEMORY,
                                _("Insufficient memory to load XBM image file"));
            return FALSE;
        }

        if (version10p) {
            unsigned char* ptr;
            guint bytes;

            /* @bytes is guaranteed not to overflow (which could
             * happen if @size is the odd-valued %G_MAXUINT: @bytes would reach
             * %G_MAXUINT-1 in the loop, then be incremented to %G_MAXUINT+1 on the
             * next iteration) because @bytes_per_line is guaranteed to be even if
             * @version10p is %TRUE (due to the selection of
             * @padding in that case), so @size must be even too. */
            g_assert((size % 2) == 0);

            for (bytes = 0, ptr = bits; bytes < size; (bytes += 2)) {
                if ((value = next_int(fstream)) < 0)
                    RETURN(FALSE);
                *(ptr++) = value;
                if (!padding || ((bytes + 2) % bytes_per_line))
                    *(ptr++) = value >> 8;
            }
        }
        else {
            unsigned char* ptr;
            guint bytes;

            for (bytes = 0, ptr = bits; bytes < size; bytes++, ptr++) {
                if ((value = next_int(fstream)) < 0)
                    RETURN(FALSE);
                *ptr = value;
            }
        }
        break;
    }

    if (!bits)
        RETURN(FALSE);

    *data = bits;
    *width = ww;
    *height = hh;
    if (x_hot)
        *x_hot = hx;
    if (y_hot)
        *y_hot = hy;

    return TRUE;
}

static GdkPixbuf* gdk_pixbuf__xbm_image_load_real(FILE* f, XBMData* context, GError** error) {
    guint w, h;
    int x_hot, y_hot;
    guchar *data = NULL, *ptr;
    guchar* pixels;
    guint row_stride;
    int x, y;
    int reg = 0; /* Quiet compiler */
    int bits;

    GdkPixbuf* pixbuf;

    if (!read_bitmap_file_data(f, &w, &h, &data, &x_hot, &y_hot, error)) {
        if (error != NULL && *error == NULL)
            g_set_error_literal(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_CORRUPT_IMAGE, _("Invalid XBM file"));
        return NULL;
    }

    pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, w, h);

    if (pixbuf == NULL) {
        g_free(data);
        g_set_error_literal(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_INSUFFICIENT_MEMORY,
                            _("Insufficient memory to load XBM image file"));
        return NULL;
    }

    if (x_hot != -1 && y_hot != -1) {
        gchar hot[10];
        g_snprintf(hot, 10, "%d", x_hot);
        gdk_pixbuf_set_option(pixbuf, "x_hot", hot);
        g_snprintf(hot, 10, "%d", y_hot);
        gdk_pixbuf_set_option(pixbuf, "y_hot", hot);
    }

    pixels = gdk_pixbuf_get_pixels(pixbuf);
    row_stride = gdk_pixbuf_get_rowstride(pixbuf);

    if (context)
        (*context->prepared_func)(pixbuf, NULL, context->user_data);

    /* Initialize PIXBUF */

    ptr = data;
    for (y = 0; y < h; y++) {
        bits = 0;
        for (x = 0; x < w; x++) {
            guchar channel;
            if (bits == 0) {
                reg = *ptr++;
                bits = 8;
            }

            channel = (reg & 1) ? 0 : 255;
            reg >>= 1;
            bits--;

            pixels[x * 3 + 0] = channel;
            pixels[x * 3 + 1] = channel;
            pixels[x * 3 + 2] = channel;
        }
        pixels += row_stride;
    }
    g_free(data);

    if (context) {
        (*context->updated_func)(pixbuf, 0, 0, w, h, context->user_data);
    }

    return pixbuf;
}

/* Static loader */

static GdkPixbuf* gdk_pixbuf__xbm_image_load(FILE* f, GError** error) {
    return gdk_pixbuf__xbm_image_load_real(f, NULL, error);
}

/* Progressive loader */

/*
 * Proper XBM progressive loading isn't implemented.  Instead we write
 * it to a file, then load the file when it's done.  It's not pretty.
 */

static gpointer gdk_pixbuf__xbm_image_begin_load(GdkPixbufModuleSizeFunc size_func,
                                                 GdkPixbufModulePreparedFunc prepared_func,
                                                 GdkPixbufModuleUpdatedFunc updated_func,
                                                 gpointer user_data,
                                                 GError** error) {
    XBMData* context;
    gint fd;

    g_assert(size_func != NULL);
    g_assert(prepared_func != NULL);
    g_assert(updated_func != NULL);

    context = g_new(XBMData, 1);
    context->prepared_func = prepared_func;
    context->updated_func = updated_func;
    context->user_data = user_data;
    context->all_okay = TRUE;
    fd = g_file_open_tmp("gdkpixbuf-xbm-tmp.XXXXXX", &context->tempname, error);
    if (fd < 0) {
        g_free(context);
        return NULL;
    }

    context->file = fdopen(fd, "w+");
    if (context->file == NULL) {
        gint save_errno = errno;
        g_set_error_literal(error, G_FILE_ERROR, g_file_error_from_errno(save_errno),
                            _("Failed to open temporary file"));
        g_close(fd, NULL);
        g_unlink(context->tempname);
        g_free(context->tempname);
        g_free(context);
        return NULL;
    }

    return context;
}

static gboolean gdk_pixbuf__xbm_image_stop_load(gpointer data, GError** error) {
    XBMData* context = (XBMData*)data;
    gboolean retval = TRUE;

    g_return_val_if_fail(data != NULL, TRUE);

    if (fflush(context->file) != 0) {
        gint save_errno = errno;
        g_set_error_literal(error, G_FILE_ERROR, g_file_error_from_errno(save_errno),
                            _("Failed to write to temporary file when loading XBM image"));
        retval = FALSE;
        goto out;
    }
    if (fseek(context->file, 0, SEEK_SET) != 0) {
        gint save_errno = errno;
        g_set_error_literal(error, G_FILE_ERROR, g_file_error_from_errno(save_errno),
                            _("Failed to rewind temporary file"));
        retval = FALSE;
        goto out;
    }
    if (context->all_okay) {
        GdkPixbuf* pixbuf;
        pixbuf = gdk_pixbuf__xbm_image_load_real(context->file, context, error);
        if (pixbuf == NULL)
            retval = FALSE;
        else
            g_object_unref(pixbuf);
    }

out:
    if (fclose(context->file) != 0 && retval) {
        gint save_errno = errno;
        g_set_error_literal(error, G_FILE_ERROR, g_file_error_from_errno(save_errno),
                            _("Failed to close temporary file"));
        retval = FALSE;
    }
    g_unlink(context->tempname);
    g_free(context->tempname);
    g_free((XBMData*)context);

    return retval;
}

static gboolean gdk_pixbuf__xbm_image_load_increment(gpointer data, const guchar* buf, guint size, GError** error) {
    XBMData* context = (XBMData*)data;

    g_return_val_if_fail(data != NULL, FALSE);

    if (fwrite(buf, sizeof(guchar), size, context->file) != size) {
        gint save_errno = errno;
        context->all_okay = FALSE;
        g_set_error_literal(error, G_FILE_ERROR, g_file_error_from_errno(save_errno),
                            _("Failed to write to temporary file when loading XBM image"));
        return FALSE;
    }

    return TRUE;
}

#ifndef INCLUDE_xbm
#define MODULE_ENTRY(function) G_MODULE_EXPORT void function
#else
#define MODULE_ENTRY(function) void _gdk_pixbuf__xbm_##function
#endif

MODULE_ENTRY(fill_vtable)(GdkPixbufModule* module) {
    module->load = gdk_pixbuf__xbm_image_load;
    module->begin_load = gdk_pixbuf__xbm_image_begin_load;
    module->stop_load = gdk_pixbuf__xbm_image_stop_load;
    module->load_increment = gdk_pixbuf__xbm_image_load_increment;
}

MODULE_ENTRY(fill_info)(GdkPixbufFormat* info) {
    static const GdkPixbufModulePattern signature[] = {{"#define ", NULL, 100}, {"/*", NULL, 50}, {NULL, NULL, 0}};
    static const gchar* mime_types[] = {"image/x-xbitmap", NULL};
    static const gchar* extensions[] = {"xbm", NULL};

    info->name = "xbm";
    info->signature = (GdkPixbufModulePattern*)signature;
    info->description = NC_("image format", "XBM");
    info->mime_types = (gchar**)mime_types;
    info->extensions = (gchar**)extensions;
    info->flags = GDK_PIXBUF_FORMAT_THREADSAFE;
    info->license = "LGPL";
}
