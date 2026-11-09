/*
 * Copyright (C) 2013 Bastien Nocera <hadess@hadess.net>
 *
 * Authors: Bastien Nocera <hadess@hadess.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#include <string.h>
#include <glib.h>
#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include <locale.h>
#include <math.h>

#include "gnome-thumbnailer-skeleton.h"

static int output_size = 256;
static gboolean g_fatal_warnings = FALSE;

static char* get_target_uri(GFile* file, GError** error) {
    GFileInfo* info;
    char* target;

    info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI, G_FILE_QUERY_INFO_NONE, NULL, error);
    if (info == NULL)
        return NULL;
    if (g_file_info_get_attribute_type(info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI) != G_FILE_ATTRIBUTE_TYPE_STRING) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Target URI is missing or is not a string");
        target = NULL;
    }
    else {
        target = g_strdup(g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI));
        if (target == NULL || *target == '\0') {
            g_clear_pointer(&target, g_free);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Target URI is missing or empty");
        }
    }
    g_object_unref(info);

    return target;
}

static char* get_target_path(GFile* input, GError** error) {
    if (g_file_has_uri_scheme(input, "trash") != FALSE || g_file_has_uri_scheme(input, "recent") != FALSE) {
        GFile* file;
        char* input_uri;
        char* input_path;

        input_uri = get_target_uri(input, error);
        if (input_uri == NULL)
            return NULL;
        file = g_file_new_for_uri(input_uri);
        g_free(input_uri);
        input_path = g_file_get_path(file);
        g_object_unref(file);
        return input_path;
    }
    return g_file_get_path(input);
}

int main(int argc, char** argv) {
    char* input_filename;
    g_autoptr(GdkPixbuf) pixbuf = NULL;
    g_autoptr(GError) error = NULL;
    g_auto(GStrv) filenames = NULL;
    g_autoptr(GOptionContext) context = NULL;
    GFile* input;
    const char* output;
    const GOptionEntry entries[] = {
        {"size", 's', 0, G_OPTION_ARG_INT, &output_size, "Size of the thumbnail in pixels", NULL},
        {"g-fatal-warnings", 0, 0, G_OPTION_ARG_NONE, &g_fatal_warnings, "Make all warnings fatal", NULL},
        {G_OPTION_REMAINING, '\0', 0, G_OPTION_ARG_FILENAME_ARRAY, &filenames, NULL, "[INPUT FILE] [OUTPUT FILE]"},
        {NULL, 0, 0, 0, NULL, NULL, NULL}};

    setlocale(LC_ALL, "");

    /* Options parsing */
    context = g_option_context_new("Thumbnail images");
    g_option_context_add_main_entries(context, entries, NULL);

    if (g_option_context_parse(context, &argc, &argv, &error) == FALSE) {
        g_warning("Couldn't parse command-line options: %s", error->message);
        return 1;
    }

    if (output_size <= 0) {
        g_printerr("Thumbnail size must be greater than zero\n");
        return 1;
    }

    /* Set fatal warnings if required */
    if (g_fatal_warnings) {
        GLogLevelFlags fatal_mask;

        fatal_mask = g_log_set_always_fatal(G_LOG_FATAL_MASK);
        fatal_mask |= G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL;
        g_log_set_always_fatal(fatal_mask);
    }

    if (filenames == NULL || g_strv_length(filenames) != 2) {
        g_print("Expects an input and an output file\n");
        return 1;
    }

    input = g_file_new_for_commandline_arg(filenames[0]);
    input_filename = get_target_path(input, &error);
    g_object_unref(input);

    if (input_filename == NULL) {
        g_warning("Could not get file path for %s: %s", filenames[0], error ? error->message : "No local file path");
        return 1;
    }

    output = filenames[1];

    pixbuf = file_to_pixbuf(input_filename, output_size, &error);
    if (pixbuf != NULL) {
        int width, height;

        width = gdk_pixbuf_get_width(pixbuf);
        height = gdk_pixbuf_get_height(pixbuf);

        /* Handle naive thumbnailers that don't resize */
        if (output_size != 0 && (height > output_size || width > output_size)) {
            GdkPixbuf* scaled;
            double scale;

            scale = (double)output_size / MAX(width, height);

            scaled = gdk_pixbuf_scale_simple(pixbuf, floor(width * scale + 0.5), floor(height * scale + 0.5),
                                             GDK_INTERP_BILINEAR);
            gdk_pixbuf_copy_options(pixbuf, scaled);
            g_object_unref(pixbuf);
            pixbuf = scaled;
        }
    }
    g_free(input_filename);

    if (!pixbuf) {
        g_warning("Could not thumbnail '%s': %s", filenames[0],
                  error ? error->message : "Thumbnailer failed without returning an error");
        return 1;
    }

    if (gdk_pixbuf_save(pixbuf, output, "png", &error, NULL) == FALSE) {
        g_warning("Couldn't save the thumbnail '%s' for file '%s': %s", output, filenames[0], error->message);
        return 1;
    }

    return 0;
}
