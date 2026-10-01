/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
   Copyright (C) 2000 Eazel, Inc.
   Copyright (C) 2002, 2003, 2004, 2005 Dom Lachowicz <cinamod@hotmail.com>
   Copyright (C) 2003, 2004, 2005 Caleb Moore <c.moore@student.unsw.edu.au>
   Copyright © 2011, 2012 Christian Persch

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with this program; if not, write to the
   Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.
*/

#include "config.h"

#include "rsvg-io.h"

#include <string.h>

/* Copied from soup-request-data.c (LGPL2+):
 * Copyright (C) 2009, 2010 Red Hat, Inc.
 * Copyright (C) 2010 Igalia, S.L.
 * and from soup-uri.c:
 * Copyright 1999-2003 Ximian, Inc.
 */

static char* uri_decoded_copy(const char* part, gsize length, gsize* out_len, GError** error) {
    char* decoded = g_try_malloc(length + 1);
    gsize written = 0;
    if (!decoded) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Cannot allocate data URI buffer");
        return NULL;
    }
    for (gsize i = 0; i < length; i++) {
        if (part[i] == '%') {
            if (length - i < 3 || !g_ascii_isxdigit(part[i + 1]) || !g_ascii_isxdigit(part[i + 2])) {
                g_free(decoded);
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid data URI escape");
                return NULL;
            }
            decoded[written++] = (g_ascii_xdigit_value(part[i + 1]) << 4) | g_ascii_xdigit_value(part[i + 2]);
            i += 2;
        }
        else {
            decoded[written++] = part[i];
        }
    }
    decoded[written] = '\0';
    *out_len = written;
    return decoded;
}

#define BASE64_INDICATOR ";base64"
#define BASE64_INDICATOR_LEN (sizeof(";base64") - 1)

static gboolean valid_base64(const char* data, gsize len) {
    gsize digits = 0;
    guint padding = 0;
    for (gsize i = 0; i < len; i++) {
        unsigned char c = data[i];
        if (g_ascii_isspace(c))
            continue;
        if (c == '=') {
            if (++padding > 2)
                return FALSE;
        }
        else if (padding || !(g_ascii_isalnum(c) || c == '+' || c == '/')) {
            return FALSE;
        }
        else {
            digits++;
        }
    }
    return (digits + padding) % 4 == 0;
}

static char* rsvg_decode_data_uri(const char* uri,
                                  char** out_mime_type,
                                  gsize* out_len,
                                  GCancellable* cancellable,
                                  GError** error) {
    const char *start = uri + 5, *comma = strchr(start, ','), *end;
    char* mime_type = NULL;
    char* data = NULL;
    gsize data_len, mime_len;
    gboolean base64 = FALSE;

    g_assert(out_len != NULL);
    g_assert(strncmp(uri, "data:", 5) == 0);
    if (g_cancellable_set_error_if_cancelled(cancellable, error))
        return NULL;
    if (!comma)
        goto invalid;
    end = comma;
    if ((gsize)(comma - start) >= BASE64_INDICATOR_LEN &&
        !g_ascii_strncasecmp(comma - BASE64_INDICATOR_LEN, BASE64_INDICATOR, BASE64_INDICATOR_LEN)) {
        end -= BASE64_INDICATOR_LEN;
        base64 = TRUE;
    }
    if (end != start) {
        mime_type = uri_decoded_copy(start, end - start, &mime_len, error);
        if (!mime_type)
            goto failed;
        if (memchr(mime_type, '\0', mime_len))
            goto invalid;
    }
    start = comma + 1;
    data = uri_decoded_copy(start, strlen(start), &data_len, error);
    if (!data)
        goto failed;
    if (base64) {
        if (!valid_base64(data, data_len))
            goto invalid;
        if (data_len)
            g_base64_decode_inplace(data, &data_len);
        data[data_len] = '\0';
    }
    if (g_cancellable_set_error_if_cancelled(cancellable, error))
        goto failed;
    if (out_mime_type)
        *out_mime_type = mime_type;
    else
        g_free(mime_type);
    *out_len = data_len;
    return data;

invalid:
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid data URI structure or base64 payload");
failed:
    g_free(mime_type);
    g_free(data);
    return NULL;
}

static char* rsvg_acquire_file_data(GFile* file,
                                    const char* uri,
                                    char** out_mime_type,
                                    gsize* out_len,
                                    GCancellable* cancellable,
                                    GError** error) {
    char* data = NULL;

    if (!g_file_load_contents(file, cancellable, &data, out_len, NULL, error))
        return NULL;

    if (out_mime_type) {
        g_autofree char* path = g_file_get_path(file);
        g_autofree char* content_type = g_content_type_guess(path ? path : uri, (guchar*)data, *out_len, NULL);
        *out_mime_type = content_type ? g_content_type_get_mime_type(content_type) : NULL;
    }

    return data;
}

static GInputStream* rsvg_acquire_file_stream(GFile* file,
                                              char** out_mime_type,
                                              GCancellable* cancellable,
                                              GError** error) {
    GFileInputStream* stream;

    stream = g_file_read(file, cancellable, error);
    if (stream == NULL)
        return NULL;

    if (out_mime_type) {
        g_autoptr(GFileInfo) file_info = g_file_input_stream_query_info(stream, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
                                                                        cancellable, NULL /* error */);
        const char* content_type = NULL;

        if (file_info && g_file_info_has_attribute(file_info, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE))
            content_type = g_file_info_get_content_type(file_info);
        *out_mime_type = content_type ? g_content_type_get_mime_type(content_type) : NULL;
    }

    return G_INPUT_STREAM(stream);
}

char* _rsvg_io_acquire_data(const char* href,
                            const char* base_uri,
                            char** mime_type,
                            gsize* len,
                            GCancellable* cancellable,
                            GError** error) {
    gsize llen;

    (void)base_uri; /* Retained in the exported ABI; href must already be resolved. */

    if (!(href && *href)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid URI");
        return NULL;
    }

    if (!len)
        len = &llen;

    if (strncmp(href, "data:", 5) == 0)
        return rsvg_decode_data_uri(href, mime_type, len, cancellable, error);

    g_autoptr(GFile) file = g_file_new_for_uri(href);
    return rsvg_acquire_file_data(file, href, mime_type, len, cancellable, error);
}

GInputStream* _rsvg_io_acquire_stream(const char* href,
                                      const char* base_uri,
                                      char** mime_type,
                                      GCancellable* cancellable,
                                      GError** error) {
    char* data;
    gsize len;

    (void)base_uri; /* Retained in the exported ABI; href must already be resolved. */

    if (!(href && *href)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid URI");
        return NULL;
    }

    if (strncmp(href, "data:", 5) == 0) {
        data = rsvg_decode_data_uri(href, mime_type, &len, cancellable, error);
    }
    else {
        g_autoptr(GFile) file = g_file_new_for_uri(href);
        if (!g_file_is_native(file))
            return rsvg_acquire_file_stream(file, mime_type, cancellable, error);

        /* Preserve native buffering and MIME sniffing, but choose the reader
         * before I/O: a failed native read must not trigger a second attempt.
         */
        data = rsvg_acquire_file_data(file, href, mime_type, &len, cancellable, error);
    }

    if (data == NULL)
        return NULL;

    return g_memory_input_stream_new_from_data(data, len, (GDestroyNotify)g_free);
}
