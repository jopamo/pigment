/* -*- mode: C; c-file-style: "linux" -*- */
/* GdkPixbuf library - QTIF image loader
 *
 * This module extracts image data from QTIF format and uses
 * other GDK pixbuf modules to decode the image data.
 *
 * Copyright (C) 2008 Kevin Peng
 *
 * Authors: Kevin Peng <kevin@zycomtech.com>
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
#include <errno.h>
#include <libintl.h>
#include <stdio.h>
#include <string.h>
#include <glib/gi18n-lib.h>
#include "gdk-pixbuf.h"

/***
 * Definitions
 */
#define READ_BUFFER_SIZE 8192
/* Only allow atoms of at most 100 MB. */
#define ATOM_SIZE_MAX 100000000

/* Aborts after going to through this many atoms. */
#define QTIF_ATOM_COUNT_MAX 10u

/* QTIF static image data tag "idat". */
#define QTIF_TAG_IDATA 0x69646174u

/***
 * Types
 */
/* QTIF State */
typedef enum { STATE_READY, STATE_DATA, STATE_OTHER, STATE_DONE } QTIFState;

/* QTIF Atom Header */
typedef struct {
    guint32 length;
    guint32 tag;
} QtHeader;

/* QTIF loader context */
typedef struct {
    GdkPixbufLoader* loader;
    gpointer user_data;
    QTIFState state;
    guint32 run_length;
    gint atom_count;

    guchar header_buffer[sizeof(QtHeader)];

    GdkPixbufModuleSizeFunc size_func;
    GdkPixbufModulePreparedFunc prepared_func;
    GdkPixbufModuleUpdatedFunc updated_func;
    gint cb_update_count;
} QTIFContext;

/***
 * Local function prototypes
 */
static gpointer gdk_pixbuf__qtif_image_begin_load(GdkPixbufModuleSizeFunc size_func,
                                                  GdkPixbufModulePreparedFunc prepared_func,
                                                  GdkPixbufModuleUpdatedFunc updated_func,
                                                  gpointer user_data,
                                                  GError** error);
static gboolean gdk_pixbuf__qtif_image_stop_load(gpointer context, GError** error);
static gboolean gdk_pixbuf__qtif_image_load_increment(gpointer context, const guchar* buf, guint size, GError** error);
static gboolean gdk_pixbuf__qtif_image_create_loader(QTIFContext* context, GError** error);
static gboolean gdk_pixbuf__qtif_image_free_loader(QTIFContext* context, GError** error);

static void gdk_pixbuf__qtif_cb_size_prepared(gpointer loader, gint width, gint height, gpointer user_data);
static void gdk_pixbuf__qtif_cb_area_prepared(GdkPixbufLoader* loader, gpointer user_data);
static void gdk_pixbuf__qtif_cb_area_updated(gpointer loader,
                                             gint x,
                                             gint y,
                                             gint width,
                                             gint height,
                                             gpointer user_data);

/***
 * Function definitions.
 */

/* Incremental load begin. */
static gpointer gdk_pixbuf__qtif_image_begin_load(GdkPixbufModuleSizeFunc size_func,
                                                  GdkPixbufModulePreparedFunc prepared_func,
                                                  GdkPixbufModuleUpdatedFunc updated_func,
                                                  gpointer user_data,
                                                  GError** error) {
    QTIFContext* context;

    g_assert(size_func != NULL);
    g_assert(prepared_func != NULL);
    g_assert(updated_func != NULL);

    /* Create context struct. */
    context = g_new0(QTIFContext, 1);
    if (context == NULL) {
        g_set_error_literal(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_INSUFFICIENT_MEMORY,
                            _("Failed to allocate QTIF context structure."));
        return NULL;
    }

    /* Fill context parameters. */
    context->loader = NULL;
    context->user_data = user_data;
    context->state = STATE_READY;
    context->run_length = 0u;
    context->atom_count = QTIF_ATOM_COUNT_MAX;
    context->size_func = size_func;
    context->prepared_func = prepared_func;
    context->updated_func = updated_func;

    return context;
}

/* Incremental load clean up. */
static gboolean gdk_pixbuf__qtif_image_stop_load(gpointer data, GError** error) {
    QTIFContext* context = (QTIFContext*)data;
    gboolean ret = TRUE;

    if (context->loader != NULL) {
        GError* tmp = NULL;

        ret = gdk_pixbuf__qtif_image_free_loader(context, &tmp);
        if (!ret && tmp != NULL) {
            g_propagate_error(error, tmp);
        }
    }
    if (ret && context->state != STATE_DONE) {
        g_set_error_literal(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                            _("Premature end-of-file encountered"));
        ret = FALSE;
    }
    g_free(context);

    return ret;
}

/* Create a new GdkPixbufLoader and connect to its signals. */
static gboolean gdk_pixbuf__qtif_image_create_loader(QTIFContext* context, GError** error) {
    if (context == NULL) {
        return FALSE;
    }

    /* Create GdkPixbufLoader object. */
    context->loader = gdk_pixbuf_loader_new();
    if (context->loader == NULL) {
        g_set_error_literal(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_FAILED,
                            _("Failed to create GdkPixbufLoader object."));
        return FALSE;
    }

    /* Connect signals. */
    context->cb_update_count = 0;
    g_signal_connect(context->loader, "size-prepared", G_CALLBACK(gdk_pixbuf__qtif_cb_size_prepared), context);
    g_signal_connect(context->loader, "area-prepared", G_CALLBACK(gdk_pixbuf__qtif_cb_area_prepared), context);
    g_signal_connect(context->loader, "area-updated", G_CALLBACK(gdk_pixbuf__qtif_cb_area_updated), context);
    return TRUE;
}

/* Free the GdkPixbufLoader and perform callback if haven't done so. */
static gboolean gdk_pixbuf__qtif_image_free_loader(QTIFContext* context, GError** error) {
    GdkPixbuf* pixbuf;
    gboolean ret;

    if ((context == NULL) || (context->loader == NULL)) {
        return FALSE;
    }

    /* Close GdkPixbufLoader. */
    ret = gdk_pixbuf_loader_close(context->loader, error);

    /* Get GdkPixbuf from GdkPixbufLoader. */
    pixbuf = gdk_pixbuf_loader_get_pixbuf(context->loader);
    if (pixbuf != NULL) {
        g_object_ref(pixbuf);
    }

    /* Free GdkPixbufLoader. */
    g_clear_object(&context->loader);

    if (pixbuf != NULL) {
        if (context->cb_update_count == 0) {
            gint width;
            gint height;

            width = gdk_pixbuf_get_width(pixbuf);
            height = gdk_pixbuf_get_height(pixbuf);
            (context->updated_func)(pixbuf, 0, 0, width, height, context->user_data);
        }

        /* Free GdkPixbuf (callback function should ref it). */
        g_object_unref(pixbuf);
    }

    return ret;
}

/* Incrementally load the next chunk of data. */
static gboolean gdk_pixbuf__qtif_image_load_increment(gpointer data, const guchar* buf, guint size, GError** error) {
    QTIFContext* context = (QTIFContext*)data;
    GError* tmp = NULL;
    gboolean ret = TRUE; /* Return TRUE for insufficient data. */

    while (ret && (size != 0u)) {
        switch (context->state) {
            case STATE_DONE:
                return TRUE;
            case STATE_READY:
                /* Abort if we have seen too many atoms. */
                if (context->atom_count == 0u) {
                    g_set_error_literal(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                                        _("Failed to find an image data atom."));
                    return FALSE;
                }

                /* Copy to header buffer in context, in case supplied data is not enough. */
                while (context->run_length < sizeof(QtHeader) && size > 0u) {
                    context->header_buffer[context->run_length] = *buf;
                    context->run_length++;
                    buf++;
                    size--;
                }

                /* Parse buffer as QT header. */
                if (context->run_length == sizeof(QtHeader)) {
                    QtHeader hdr;
                    memcpy(&hdr, context->header_buffer, sizeof hdr);
                    hdr.length = GUINT32_FROM_BE(hdr.length);
                    hdr.tag = GUINT32_FROM_BE(hdr.tag);
                    context->atom_count--;
                    if (hdr.length < sizeof(QtHeader)) {
                        g_set_error_literal(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                                            _("Failed to read QTIF header"));
                        return FALSE;
                    }
                    context->run_length = hdr.length - sizeof(QtHeader);

                    /* Atom max size check. */
                    if (context->run_length > ATOM_SIZE_MAX) {
                        g_set_error(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                                    ngettext("QTIF atom size too large (%u byte)",
                                             "QTIF atom size too large (%u bytes)", context->run_length),
                                    context->run_length);
                        return FALSE;
                    }

                    /* Set state according to atom type. */
                    if (hdr.tag == QTIF_TAG_IDATA) {
                        GError* tmp = NULL;

                        context->state = STATE_DATA;

                        /* Create GdkPixbufLoader for this image data. */
                        ret = gdk_pixbuf__qtif_image_create_loader(context, &tmp);
                        if (!ret) {
                            g_propagate_error(error, tmp);
                        }
                    }
                    else {
                        context->state = STATE_OTHER;
                    }
                }
                break;

            default: /* Both STATE_DATA and STATE_OTHER will come here. */
                /* Check for atom boundary. */
                if (context->run_length > size) {
                    /* Supply image data to GdkPixbufLoader if in STATE_DATA. */
                    if (context->state == STATE_DATA) {
                        tmp = NULL;
                        ret = gdk_pixbuf_loader_write(context->loader, buf, size, &tmp);
                        if (!ret) {
                            if (error != NULL && *error == NULL && tmp != NULL)
                                g_propagate_error(error, g_steal_pointer(&tmp));
                            g_clear_error(&tmp);
                        }
                    }
                    context->run_length -= size;
                    size = 0u;
                }
                else {
                    /* Supply image data to GdkPixbufLoader if in STATE_DATA. */
                    if (context->state == STATE_DATA) {
                        gboolean r;

                        /* Here we should have concluded a complete image atom. */
                        tmp = NULL;
                        ret = gdk_pixbuf_loader_write(context->loader, buf, context->run_length, &tmp);
                        if (!ret) {
                            if (error != NULL && *error == NULL && tmp != NULL)
                                g_propagate_error(error, g_steal_pointer(&tmp));
                            g_clear_error(&tmp);
                        }

                        /* Free GdkPixbufLoader and handle callback. */
                        tmp = NULL;
                        r = gdk_pixbuf__qtif_image_free_loader(context, &tmp);
                        if (!r) {
                            if (error != NULL && *error == NULL && tmp != NULL)
                                g_propagate_error(error, g_steal_pointer(&tmp));
                            g_clear_error(&tmp);
                            ret = FALSE;
                        }
                        if (ret) {
                            context->state = STATE_DONE;
                            return TRUE;
                        }
                    }
                    buf = &buf[context->run_length];
                    size -= context->run_length;
                    context->run_length = 0u;
                    context->state = STATE_READY;
                }
                break;
        }
    }

    return ret;
}

static void file_size(gint* width, gint* height, gpointer data) {}

static void file_prepared(GdkPixbuf* pixbuf, GdkPixbufAnimation* animation, gpointer data) {
    GdkPixbuf** result = data;
    *result = g_object_ref(pixbuf);
}

static void file_updated(GdkPixbuf* pixbuf, gint x, gint y, gint width, gint height, gpointer data) {}

static GdkPixbuf* gdk_pixbuf__qtif_image_load(FILE* file, GError** error) {
    GdkPixbuf* pixbuf = NULL;
    QTIFContext* context;
    guchar buffer[READ_BUFFER_SIZE];
    gboolean success = TRUE;

    if (file == NULL) {
        g_set_error_literal(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_BAD_OPTION, _("Input file descriptor is NULL."));
        return NULL;
    }
    context = gdk_pixbuf__qtif_image_begin_load(file_size, file_prepared, file_updated, &pixbuf, error);
    if (context == NULL)
        return NULL;

    while (context->state != STATE_DONE) {
        guint count = context->state == STATE_READY ? sizeof(QtHeader) - context->run_length
                                                    : MAX(1, MIN(context->run_length, sizeof buffer));
        if (fread(buffer, 1, count, file) != count) {
            if (ferror(file))
                g_set_error(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                            _("File error when reading QTIF atom: %s"), g_strerror(errno));
            else
                g_set_error_literal(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                                    _("Premature end-of-file encountered"));
            success = FALSE;
            break;
        }
        if (!gdk_pixbuf__qtif_image_load_increment(context, buffer, count, error)) {
            success = FALSE;
            break;
        }
    }
    if (!gdk_pixbuf__qtif_image_stop_load(context, success ? error : NULL))
        success = FALSE;
    if (!success)
        g_clear_object(&pixbuf);
    return pixbuf;
}

/* Event handlers */
static void gdk_pixbuf__qtif_cb_size_prepared(gpointer loader, gint width, gint height, gpointer user_data) {
    QTIFContext* context = (QTIFContext*)user_data;
    (void)loader;
    (context->size_func)(&width, &height, context->user_data);
}

static void gdk_pixbuf__qtif_cb_area_prepared(GdkPixbufLoader* loader, gpointer user_data) {
    QTIFContext* context = (QTIFContext*)user_data;
    GdkPixbuf* pixbuf = gdk_pixbuf_loader_get_pixbuf(context->loader);
    (context->prepared_func)(pixbuf, NULL, context->user_data);
}

static void gdk_pixbuf__qtif_cb_area_updated(gpointer loader,
                                             gint x,
                                             gint y,
                                             gint width,
                                             gint height,
                                             gpointer user_data) {
    QTIFContext* context = (QTIFContext*)user_data;
    GdkPixbuf* pixbuf = gdk_pixbuf_loader_get_pixbuf(context->loader);
    context->cb_update_count++;
    (context->updated_func)(pixbuf, x, y, width, height, context->user_data);
}

#ifndef INCLUDE_qtif
#define MODULE_ENTRY(function) G_MODULE_EXPORT void function
#else
#define MODULE_ENTRY(function) void _gdk_pixbuf__qtif_##function
#endif

MODULE_ENTRY(fill_vtable)(GdkPixbufModule* module) {
    module->load = gdk_pixbuf__qtif_image_load;
    module->begin_load = gdk_pixbuf__qtif_image_begin_load;
    module->stop_load = gdk_pixbuf__qtif_image_stop_load;
    module->load_increment = gdk_pixbuf__qtif_image_load_increment;
}

MODULE_ENTRY(fill_info)(GdkPixbufFormat* info) {
    static const GdkPixbufModulePattern signature[] = {
        {"abcdidsc", "xxxx    ", 100}, {"abcdidat", "xxxx    ", 100}, {NULL, NULL, 0}};
    static const gchar* mime_types[] = {"image/x-quicktime", "image/qtif", NULL};
    static const gchar* extensions[] = {"qtif", "qif", NULL};

    info->name = "qtif";
    info->signature = (GdkPixbufModulePattern*)signature;
    info->description = NC_("image format", "QuickTime");
    info->mime_types = (gchar**)mime_types;
    info->extensions = (gchar**)extensions;
    info->flags = GDK_PIXBUF_FORMAT_THREADSAFE;
    info->license = "LGPL";
}
