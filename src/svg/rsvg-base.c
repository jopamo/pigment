/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* vim: set sw=4 sts=4 ts=4 expandtab: */
/*
   rsvg.c: SAX-based renderer for SVG files into a GdkPixbuf.

   Copyright (C) 2000 Eazel, Inc.
   Copyright (C) 2002 Dom Lachowicz <cinamod@hotmail.com>

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

   Author: Raph Levien <raph@artofcode.com>
*/

#include "config.h"
#define _GNU_SOURCE 1

#ifdef HAVE_RSVG_ROOTED_FILE_IO
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <gio/gunixinputstream.h>
#endif

#include "rsvg.h"
#include "rsvg-private.h"
#include "rsvg-css.h"
#include "rsvg-css-engine.h"
#include "rsvg-styles.h"
#include "rsvg-shapes.h"
#include "rsvg-structure.h"
#include "rsvg-image.h"
#include "rsvg-io.h"
#include "rsvg-text.h"
#include "rsvg-filter.h"
#include "rsvg-mask.h"
#include "rsvg-marker.h"
#include "rsvg-cairo-render.h"

#include <libxml/uri.h>
#include <libxml/parser.h>
#include <libxml/parserInternals.h>

#include <gio/gio.h>

#include <math.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <stdlib.h>

#include "rsvg-path.h"
#include "rsvg-paint-server.h"
#include "rsvg-xml.h"

/*
 * This is configurable at runtime
 */
#define RSVG_DEFAULT_DPI_X 90.0
#define RSVG_DEFAULT_DPI_Y 90.0

#define RSVG_XINCLUDE_MAX_DEPTH 16
#define RSVG_XINCLUDE_NS_URI "http://www.w3.org/2001/XInclude"
#define RSVG_XML_NS_URI "http://www.w3.org/XML/1998/namespace"

static const char* get_localname(const char* name) {
    const char* p = strrchr(name, ':');
    return p ? p + 1 : name;
}

typedef struct {
    GHashTable* frame;
} RsvgXmlNsCollectCtx;

static void rsvg_collect_xmlns_declaration(gpointer raw_key, gpointer raw_value, gpointer user_data) {
    const char* key = raw_key;
    const char* value = raw_value;
    RsvgXmlNsCollectCtx* ctx = (RsvgXmlNsCollectCtx*)user_data;
    const char* prefix = NULL;

    if (strcmp(key, "xmlns") == 0) {
        prefix = "";
    }
    else if (g_str_has_prefix(key, "xmlns:") && key[6] != '\0') {
        prefix = key + 6;
    }
    else {
        return;
    }

    g_hash_table_insert(ctx->frame, g_strdup(prefix), g_strdup(value ? value : ""));
}

static void rsvg_push_xmlns_frame(RsvgHandle* ctx, RsvgPropertyBag* atts) {
    GHashTable* frame;
    RsvgXmlNsCollectCtx collect_ctx;

    frame = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    collect_ctx.frame = frame;
    g_hash_table_foreach(atts, rsvg_collect_xmlns_declaration, &collect_ctx);

    ctx->priv->xml_ns_stack = g_slist_prepend(ctx->priv->xml_ns_stack, frame);
}

static void rsvg_pop_xmlns_frame(RsvgHandle* ctx) {
    GSList* top;
    GHashTable* frame;

    top = ctx->priv->xml_ns_stack;
    if (top == NULL) {
        return;
    }

    frame = (GHashTable*)top->data;
    g_hash_table_unref(frame);
    ctx->priv->xml_ns_stack = g_slist_delete_link(ctx->priv->xml_ns_stack, top);
}

static void rsvg_clear_xmlns_stack(RsvgHandle* ctx) {
    while (ctx->priv->xml_ns_stack != NULL) {
        rsvg_pop_xmlns_frame(ctx);
    }
}

static const char* rsvg_lookup_namespace_uri(RsvgHandle* ctx, const char* qname) {
    const char* colon;
    char* prefix_copy = NULL;
    const char* prefix = "";
    GSList* l;

    colon = strchr(qname, ':');
    if (colon != NULL) {
        prefix_copy = g_strndup(qname, (gsize)(colon - qname));
        prefix = prefix_copy;
    }

    for (l = ctx->priv->xml_ns_stack; l != NULL; l = l->next) {
        GHashTable* frame = (GHashTable*)l->data;
        const char* uri = g_hash_table_lookup(frame, prefix);

        if (uri != NULL) {
            g_free(prefix_copy);
            return uri[0] != '\0' ? uri : NULL;
        }
    }

    g_free(prefix_copy);

    if (colon != NULL && strncmp(qname, "xml:", 4) == 0) {
        return RSVG_XML_NS_URI;
    }

    return NULL;
}

static gboolean rsvg_is_xinclude_element(RsvgHandle* ctx, const char* name, const char* local_name) {
    const char* uri;

    if (strcmp(get_localname(name), local_name) != 0) {
        return FALSE;
    }

    uri = rsvg_lookup_namespace_uri(ctx, name);
    return uri != NULL && strcmp(uri, RSVG_XINCLUDE_NS_URI) == 0;
}

G_GNUC_INTERNAL
double rsvg_internal_dpi_x = RSVG_DEFAULT_DPI_X;
G_GNUC_INTERNAL
double rsvg_internal_dpi_y = RSVG_DEFAULT_DPI_Y;

typedef struct _RsvgSaxHandlerStyle {
    RsvgSaxHandler super;
    RsvgHandle* ctx;
    GString* style;
    gboolean is_text_css;
} RsvgSaxHandlerStyle;

typedef struct {
    RsvgSaxHandler super;
    RsvgHandle* ctx;
    const char* name;
    GString* string;
    GString** stringptr;
} RsvgSaxHandlerExtra;

/* hide this fact from the general public */
typedef RsvgSaxHandlerExtra RsvgSaxHandlerTitle;
typedef RsvgSaxHandlerExtra RsvgSaxHandlerDesc;
typedef RsvgSaxHandlerExtra RsvgSaxHandlerMetadata;

static void rsvg_sax_handler_pop(RsvgHandle* ctx, gboolean finish) {
    RsvgSaxHandler* handler = ctx->priv->handler;
    ctx->priv->handler = handler->parent;
    if (finish && handler->finish)
        handler->finish(handler);
    handler->free(handler);
}

void rsvg_handle_clear_sax_handlers(RsvgHandle* handle) {
    while (handle->priv->handler)
        rsvg_sax_handler_pop(handle, FALSE);
    handle->priv->handler_nest = 0;
}

static void rsvg_style_handler_finish(RsvgSaxHandler* self) {
    RsvgSaxHandlerStyle* z = (RsvgSaxHandlerStyle*)self;
    if (z->is_text_css)
        rsvg_parse_cssbuffer(z->ctx, z->style->str, z->style->len);
}

static void rsvg_style_handler_free(RsvgSaxHandler* self) {
    RsvgSaxHandlerStyle* z = (RsvgSaxHandlerStyle*)self;
    g_string_free(z->style, TRUE);
    g_free(z);
}

static void rsvg_style_handler_characters(RsvgSaxHandler* self, const char* ch, int len) {
    RsvgSaxHandlerStyle* z = (RsvgSaxHandlerStyle*)self;
    g_string_append_len(z->style, ch, len);
}

static void rsvg_style_handler_start(RsvgSaxHandler* self, const char* name, RsvgPropertyBag* atts) {
    (void)self;
    (void)name;
    (void)atts;
}

static void rsvg_style_handler_end(RsvgSaxHandler* self, const char* name) {
    RsvgSaxHandlerStyle* z = (RsvgSaxHandlerStyle*)self;
    RsvgHandle* ctx = z->ctx;

    if (!strcmp(name, "style")) {
        if (ctx->priv->handler != NULL)
            rsvg_sax_handler_pop(ctx, TRUE);
    }
}

static void rsvg_start_style(RsvgHandle* ctx, RsvgPropertyBag* atts) {
    RsvgSaxHandlerStyle* handler = g_new0(RsvgSaxHandlerStyle, 1);
    const char* type;

    type = rsvg_property_bag_lookup(atts, "type");

    handler->super.parent = ctx->priv->handler;
    handler->super.finish = rsvg_style_handler_finish;
    handler->super.free = rsvg_style_handler_free;
    handler->super.characters = rsvg_style_handler_characters;
    handler->super.start_element = rsvg_style_handler_start;
    handler->super.end_element = rsvg_style_handler_end;
    handler->ctx = ctx;

    handler->style = g_string_new(NULL);

    /* FIXME: See these:
     *
     * https://www.w3.org/TR/SVG/styling.html#StyleElementTypeAttribute
     * https://www.w3.org/TR/SVG/styling.html#ContentStyleTypeAttribute
     *
     * If the "type" attribute is not present, we should fallback to the
     * "contentStyleType" attribute of the svg element, which in turn
     * defaults to "text/css".
     *
     * See where is_text_css is used to see where we parse the contents
     * of the style element.
     */
    handler->is_text_css = (type == NULL) || (g_ascii_strcasecmp(type, "text/css") == 0);

    ctx->priv->handler = &handler->super;
}

static void rsvg_standard_element_start(RsvgHandle* ctx, const char* name, RsvgPropertyBag* atts) {
    /*replace this stuff with a hash for fast reading! */
    RsvgNode* newnode = NULL;
    if (!strcmp(name, "g"))
        newnode = rsvg_new_group();
    else if (!strcmp(name, "a")) /*treat anchors as groups for now */
        newnode = rsvg_new_group();
    else if (!strcmp(name, "switch"))
        newnode = rsvg_new_switch();
    else if (!strcmp(name, "defs"))
        newnode = rsvg_new_defs();
    else if (!strcmp(name, "use"))
        newnode = rsvg_new_use();
    else if (!strcmp(name, "path"))
        newnode = rsvg_new_path();
    else if (!strcmp(name, "line"))
        newnode = rsvg_new_line();
    else if (!strcmp(name, "rect"))
        newnode = rsvg_new_rect();
    else if (!strcmp(name, "ellipse"))
        newnode = rsvg_new_ellipse();
    else if (!strcmp(name, "circle"))
        newnode = rsvg_new_circle();
    else if (!strcmp(name, "polygon"))
        newnode = rsvg_new_polygon();
    else if (!strcmp(name, "polyline"))
        newnode = rsvg_new_polyline();
    else if (!strcmp(name, "symbol"))
        newnode = rsvg_new_symbol();
    else if (!strcmp(name, "svg"))
        newnode = rsvg_new_svg();
    else if (!strcmp(name, "mask"))
        newnode = rsvg_new_mask();
    else if (!strcmp(name, "clipPath"))
        newnode = rsvg_new_clip_path();
    else if (!strcmp(name, "image"))
        newnode = rsvg_new_image();
    else if (!strcmp(name, "marker"))
        newnode = rsvg_new_marker();
    else if (!strcmp(name, "stop"))
        newnode = rsvg_new_stop();
    else if (!strcmp(name, "pattern"))
        newnode = rsvg_new_pattern();
    else if (!strcmp(name, "linearGradient"))
        newnode = rsvg_new_linear_gradient();
    else if (!strcmp(name, "radialGradient"))
        newnode = rsvg_new_radial_gradient();
    else if (!strcmp(name, "conicalGradient"))
        newnode = rsvg_new_radial_gradient();
    else if (!strcmp(name, "filter"))
        newnode = rsvg_new_filter();
    else if (!strcmp(name, "feBlend"))
        newnode = rsvg_new_filter_primitive_blend();
    else if (!strcmp(name, "feColorMatrix"))
        newnode = rsvg_new_filter_primitive_color_matrix();
    else if (!strcmp(name, "feComponentTransfer"))
        newnode = rsvg_new_filter_primitive_component_transfer();
    else if (!strcmp(name, "feComposite"))
        newnode = rsvg_new_filter_primitive_composite();
    else if (!strcmp(name, "feConvolveMatrix"))
        newnode = rsvg_new_filter_primitive_convolve_matrix();
    else if (!strcmp(name, "feDiffuseLighting"))
        newnode = rsvg_new_filter_primitive_diffuse_lighting();
    else if (!strcmp(name, "feDisplacementMap"))
        newnode = rsvg_new_filter_primitive_displacement_map();
    else if (!strcmp(name, "feFlood"))
        newnode = rsvg_new_filter_primitive_flood();
    else if (!strcmp(name, "feGaussianBlur"))
        newnode = rsvg_new_filter_primitive_gaussian_blur();
    else if (!strcmp(name, "feImage"))
        newnode = rsvg_new_filter_primitive_image();
    else if (!strcmp(name, "feMerge"))
        newnode = rsvg_new_filter_primitive_merge();
    else if (!strcmp(name, "feMorphology"))
        newnode = rsvg_new_filter_primitive_erode();
    else if (!strcmp(name, "feOffset"))
        newnode = rsvg_new_filter_primitive_offset();
    else if (!strcmp(name, "feSpecularLighting"))
        newnode = rsvg_new_filter_primitive_specular_lighting();
    else if (!strcmp(name, "feTile"))
        newnode = rsvg_new_filter_primitive_tile();
    else if (!strcmp(name, "feTurbulence"))
        newnode = rsvg_new_filter_primitive_turbulence();
    else if (!strcmp(name, "feMergeNode"))
        newnode = rsvg_new_filter_primitive_merge_node();
    else if (!strcmp(name, "feFuncR"))
        newnode = rsvg_new_node_component_transfer_function(
            'r'); /* See rsvg_filter_primitive_component_transfer_render() for where these values are used */
    else if (!strcmp(name, "feFuncG"))
        newnode = rsvg_new_node_component_transfer_function('g');
    else if (!strcmp(name, "feFuncB"))
        newnode = rsvg_new_node_component_transfer_function('b');
    else if (!strcmp(name, "feFuncA"))
        newnode = rsvg_new_node_component_transfer_function('a');
    else if (!strcmp(name, "feDistantLight"))
        newnode = rsvg_new_node_light_source('d');
    else if (!strcmp(name, "feSpotLight"))
        newnode = rsvg_new_node_light_source('s');
    else if (!strcmp(name, "fePointLight"))
        newnode = rsvg_new_node_light_source('p');
    /* hack to make multiImage sort-of work */
    else if (!strcmp(name, "multiImage"))
        newnode = rsvg_new_switch();
    else if (!strcmp(name, "subImageRef"))
        newnode = rsvg_new_image();
    else if (!strcmp(name, "subImage"))
        newnode = rsvg_new_group();
    else if (!strcmp(name, "text"))
        newnode = rsvg_new_text();
    else if (!strcmp(name, "tspan"))
        newnode = rsvg_new_tspan();
    else {
        /* Whenever we encounter a node we don't understand, represent it as a defs.
         * This is like a group, but it doesn't do any rendering of children.  The
         * effect is that we will ignore all children of unknown elements.
         */
        newnode = rsvg_new_defs();
    }

    if (newnode) {
        g_assert(RSVG_NODE_TYPE(newnode) != RSVG_NODE_TYPE_INVALID);
        newnode->name = g_strdup(name);
        newnode->parent = ctx->priv->currentnode;

        if (atts) {
            const char* v;
            if ((v = rsvg_property_bag_lookup(atts, "id")))
                newnode->id = g_strdup(v);
            if ((v = rsvg_property_bag_lookup(atts, "class")))
                newnode->klass = g_strdup(v);
            if ((v = rsvg_property_bag_lookup(atts, "style")))
                newnode->style_attr = g_strdup(v);
            newnode->has_style_info = 1;
        }

        rsvg_node_set_atts(newnode, ctx, atts);
        rsvg_defs_register_memory(ctx->priv->defs, newnode);
        if (ctx->priv->currentnode) {
            rsvg_node_group_pack(ctx->priv->currentnode, newnode);
            ctx->priv->currentnode = newnode;
        }
        else if (RSVG_NODE_TYPE(newnode) == RSVG_NODE_TYPE_SVG) {
            ctx->priv->treebase = newnode;
            ctx->priv->currentnode = newnode;
        }
    }
}

/* extra (title, desc, metadata) */

static void rsvg_extra_handler_finish(RsvgSaxHandler* self) {
    RsvgSaxHandlerExtra* z = (RsvgSaxHandlerExtra*)self;

    if (z->stringptr) {
        if (*z->stringptr)
            g_string_free(*z->stringptr, TRUE);
        *z->stringptr = z->string;
        z->string = NULL;
    }
}

static void rsvg_extra_handler_free(RsvgSaxHandler* self) {
    RsvgSaxHandlerExtra* z = (RsvgSaxHandlerExtra*)self;
    if (z->string) {
        g_string_free(z->string, TRUE);
    }

    g_free(self);
}

static void rsvg_extra_handler_characters(RsvgSaxHandler* self, const char* ch, int len) {
    RsvgSaxHandlerExtra* z = (RsvgSaxHandlerExtra*)self;

    /* This isn't quite the correct behavior - in theory, any graphics
       element may contain a title, desc, or metadata element */

    if (!z->string)
        return;

    if (!ch || !len)
        return;

    if (!g_utf8_validate((char*)ch, len, NULL)) {
        char* utf8;
        utf8 = rsvg_make_valid_utf8((char*)ch, len);
        g_string_append(z->string, utf8);
        g_free(utf8);
    }
    else {
        g_string_append_len(z->string, (char*)ch, len);
    }
}

static void rsvg_extra_handler_start(RsvgSaxHandler* self, const char* name, RsvgPropertyBag* atts) {
    (void)self;
    (void)name;
    (void)atts;
}

static void rsvg_extra_handler_end(RsvgSaxHandler* self, const char* name) {
    RsvgSaxHandlerExtra* z = (RsvgSaxHandlerExtra*)self;
    RsvgHandle* ctx = z->ctx;

    if (!strcmp(name, z->name)) {
        if (ctx->priv->handler != NULL)
            rsvg_sax_handler_pop(ctx, TRUE);
    }
}

static RsvgSaxHandlerExtra* rsvg_start_extra(RsvgHandle* ctx, const char* name, GString** stringptr) {
    RsvgSaxHandlerExtra* handler = g_new0(RsvgSaxHandlerExtra, 1);
    RsvgNode* treebase = ctx->priv->treebase;
    RsvgNode* currentnode = ctx->priv->currentnode;
    gboolean do_care;

    /* only parse <extra> for the <svg> node.
     * This isn't quite the correct behavior - any graphics
     * element may contain a <extra> element.
     */
    do_care = treebase != NULL && treebase == currentnode;

    handler->super.parent = ctx->priv->handler;
    handler->super.finish = rsvg_extra_handler_finish;
    handler->super.free = rsvg_extra_handler_free;
    handler->super.characters = rsvg_extra_handler_characters;
    handler->super.start_element = rsvg_extra_handler_start;
    handler->super.end_element = rsvg_extra_handler_end;
    handler->ctx = ctx;
    handler->name = name; /* interned */
    handler->string = do_care ? g_string_new(NULL) : NULL;
    handler->stringptr = do_care ? stringptr : NULL;

    ctx->priv->handler = &handler->super;

    return handler;
}

/* start desc */

static void rsvg_start_desc(RsvgHandle* ctx) {
    rsvg_start_extra(ctx, "desc", &ctx->priv->desc);
}

/* end desc */

/* start title */

static void rsvg_start_title(RsvgHandle* ctx) {
    rsvg_start_extra(ctx, "title", &ctx->priv->title);
}

/* end title */

/* start metadata */

static void rsvg_metadata_props_enumerate(gpointer raw_key, gpointer raw_value, gpointer user_data) {
    const char* key = raw_key;
    const char* value = raw_value;
    GString* metadata = (GString*)user_data;
    char* escaped = g_markup_escape_text(value, -1);
    g_string_append_printf(metadata, "%s=\"%s\" ", key, escaped);
    g_free(escaped);
}

static void rsvg_metadata_handler_start(RsvgSaxHandler* self, const char* name, RsvgPropertyBag* atts) {
    RsvgSaxHandlerMetadata* z = (RsvgSaxHandlerMetadata*)self;

    rsvg_extra_handler_start(self, name, atts);

    if (!z->string)
        return;

    g_string_append_printf(z->string, "<%s ", name);
    g_hash_table_foreach(atts, rsvg_metadata_props_enumerate, z->string);
    g_string_append(z->string, ">\n");
}

static void rsvg_metadata_handler_end(RsvgSaxHandler* self, const char* name) {
    RsvgSaxHandlerMetadata* z = (RsvgSaxHandlerMetadata*)self;

    if (strcmp(name, z->name) != 0) {
        if (z->string)
            g_string_append_printf(z->string, "</%s>\n", name);
    }
    else {
        rsvg_extra_handler_end(self, name);
    }
}

static void rsvg_start_metadata(RsvgHandle* ctx) {
    RsvgSaxHandlerMetadata* handler = rsvg_start_extra(ctx, "metadata", &ctx->priv->metadata);

    handler->super.start_element = rsvg_metadata_handler_start;
    handler->super.end_element = rsvg_metadata_handler_end;
}

/* end metadata */

/* start xinclude */

typedef struct _RsvgSaxHandlerXinclude {
    RsvgSaxHandler super;

    RsvgHandle* ctx;
    gboolean success;
    gboolean in_fallback;
} RsvgSaxHandlerXinclude;

static void rsvg_start_xinclude(RsvgHandle* ctx, RsvgPropertyBag* atts);
static void rsvg_characters_impl(RsvgHandle* ctx, const xmlChar* ch, int len);

static void rsvg_xinclude_handler_free(RsvgSaxHandler* self) {
    g_free(self);
}

static void rsvg_xinclude_handler_characters(RsvgSaxHandler* self, const char* ch, int len) {
    RsvgSaxHandlerXinclude* z = (RsvgSaxHandlerXinclude*)self;

    if (z->in_fallback) {
        rsvg_characters_impl(z->ctx, (const xmlChar*)ch, len);
    }
}

static void rsvg_xinclude_handler_start(RsvgSaxHandler* self, const char* name, RsvgPropertyBag* atts) {
    RsvgSaxHandlerXinclude* z = (RsvgSaxHandlerXinclude*)self;
    RsvgHandle* ctx = z->ctx;
    const char* local_name = get_localname(name);

    if (!z->success) {
        if (z->in_fallback) {
            if (!strcmp(local_name, "style"))
                rsvg_start_style(ctx, atts);
            else if (!strcmp(local_name, "title"))
                rsvg_start_title(ctx);
            else if (!strcmp(local_name, "desc"))
                rsvg_start_desc(ctx);
            else if (!strcmp(local_name, "metadata"))
                rsvg_start_metadata(ctx);
            else if (rsvg_is_xinclude_element(ctx, name, "include"))
                rsvg_start_xinclude(ctx, atts);
            else
                rsvg_standard_element_start(ctx, local_name, atts);
        }
        else if (rsvg_is_xinclude_element(ctx, name, "fallback")) {
            z->in_fallback = TRUE;
        }
    }
}

static void rsvg_xinclude_handler_end(RsvgSaxHandler* self, const char* name) {
    RsvgSaxHandlerXinclude* z = (RsvgSaxHandlerXinclude*)self;
    RsvgHandle* ctx = z->ctx;
    const char* local_name = get_localname(name);

    if (rsvg_is_xinclude_element(ctx, name, "include")) {
        if (ctx->priv->handler != NULL)
            rsvg_sax_handler_pop(ctx, TRUE);
    }
    else if (z->in_fallback) {
        if (rsvg_is_xinclude_element(ctx, name, "fallback"))
            z->in_fallback = FALSE;
        else {
            gboolean closed_root_svg = FALSE;

            if (ctx->priv->currentnode != NULL) {
                closed_root_svg = (ctx->priv->currentnode == ctx->priv->treebase) &&
                                  !strcmp(local_name, ctx->priv->currentnode->name);
            }

            if (ctx->priv->currentnode && !strcmp(local_name, ctx->priv->currentnode->name))
                rsvg_pop_def_group(ctx);

            if (closed_root_svg)
                _rsvg_node_svg_apply_atts((RsvgNodeSvg*)ctx->priv->treebase, ctx);
        }
    }
}

static xmlParserCtxtPtr create_xml_push_parser(RsvgHandle* handle, const char* base_uri) {
    xmlParserCtxtPtr parser;

    parser = xmlCreatePushParserCtxt(rsvg_xml_get_sax_handler(), handle, NULL, 0, base_uri);
    rsvg_xml_configure_parser(parser, handle);

    return parser;
}

static xmlParserCtxtPtr create_xml_stream_parser(RsvgHandle* handle,
                                                 GInputStream* stream,
                                                 GCancellable* cancellable,
                                                 GError** error) {
    xmlParserCtxtPtr parser;

    parser = rsvg_create_xml_parser_from_stream(rsvg_xml_get_sax_handler(), handle, stream, cancellable, error);
    if (parser) {
        rsvg_xml_configure_parser(parser, handle);
    }

    return parser;
}

static gboolean rsvg_load_xinclude_xml(RsvgHandle* ctx, const char* url) {
    GInputStream* stream;
    GError* error = NULL;
    xmlParserCtxtPtr parser, old_parser;
    RsvgSaxHandler* old_handler;
    int old_handler_nest;
    gboolean success = FALSE;
    GFile *old_base_gfile, *new_base_gfile;
    GSList* old_xml_ns_stack;
    char* uri;

    if (ctx->priv->xinclude_depth >= RSVG_XINCLUDE_MAX_DEPTH)
        return FALSE;

    uri = rsvg_handle_resolve_uri(ctx, url);
    if (!uri)
        return FALSE;

    stream = _rsvg_handle_acquire_stream(ctx, uri, NULL, &error);
    if (!stream) {
        if (error)
            g_error_free(error);
        g_free(uri);
        return FALSE;
    }

    new_base_gfile = g_file_new_for_uri(uri);
    g_free(uri);

    old_parser = ctx->priv->ctxt;
    old_handler = ctx->priv->handler;
    old_handler_nest = ctx->priv->handler_nest;
    old_base_gfile = ctx->priv->base_gfile;
    old_xml_ns_stack = ctx->priv->xml_ns_stack;
    ctx->priv->xml_ns_stack = NULL;
    if (old_base_gfile)
        g_object_ref(old_base_gfile);

    ctx->priv->handler = NULL;
    ctx->priv->handler_nest = 0;
    ctx->priv->xinclude_depth++;
    if (ctx->priv->base_gfile)
        g_object_unref(ctx->priv->base_gfile);
    ctx->priv->base_gfile = g_object_ref(new_base_gfile);

    parser = create_xml_stream_parser(ctx, stream, ctx->priv->cancellable, &error);
    if (parser) {
        ctx->priv->ctxt = parser;
        if (xmlParseDocument(parser) == 0) {
            success = TRUE;
        }
        parser = rsvg_free_xml_parser_and_doc(parser);
    }

    rsvg_handle_clear_sax_handlers(ctx);
    ctx->priv->ctxt = old_parser;
    ctx->priv->handler = old_handler;
    ctx->priv->handler_nest = old_handler_nest;
    ctx->priv->xinclude_depth--;
    rsvg_clear_xmlns_stack(ctx);
    ctx->priv->xml_ns_stack = old_xml_ns_stack;
    if (ctx->priv->base_gfile)
        g_object_unref(ctx->priv->base_gfile);
    ctx->priv->base_gfile = old_base_gfile;

    g_object_unref(new_base_gfile);
    g_object_unref(stream);
    if (error)
        g_error_free(error);

    return success;
}

static gboolean rsvg_load_xinclude_text(RsvgHandle* ctx, const char* url) {
    char* data;
    gsize len;
    GError* error = NULL;
    char* uri;

    uri = rsvg_handle_resolve_uri(ctx, url);
    if (!uri)
        return FALSE;

    data = _rsvg_handle_acquire_data(ctx, uri, NULL, &len, &error);
    g_free(uri);

    if (data) {
        rsvg_characters_impl(ctx, (const xmlChar*)data, (int)len);
        g_free(data);
        return TRUE;
    }
    if (error)
        g_error_free(error);
    return FALSE;
}

typedef enum { RSVG_XINCLUDE_PARSE_XML, RSVG_XINCLUDE_PARSE_TEXT, RSVG_XINCLUDE_PARSE_INVALID } RsvgXincludeParseType;

static RsvgXincludeParseType rsvg_classify_xinclude_parse(const char* parse) {
    char *token = NULL, *lower = NULL;
    char* semicolon;
    RsvgXincludeParseType parse_type = RSVG_XINCLUDE_PARSE_INVALID;

    if (parse == NULL || g_ascii_strcasecmp(parse, "xml") == 0) {
        return RSVG_XINCLUDE_PARSE_XML;
    }

    if (g_ascii_strcasecmp(parse, "text") == 0) {
        return RSVG_XINCLUDE_PARSE_TEXT;
    }

    token = g_strdup(parse);
    g_strstrip(token);
    semicolon = strchr(token, ';');
    if (semicolon != NULL) {
        *semicolon = '\0';
    }
    g_strstrip(token);

    if (token[0] == '\0') {
        goto out;
    }

    lower = g_ascii_strdown(token, -1);

    if (strcmp(lower, "application/xml") == 0 || strcmp(lower, "text/xml") == 0 || g_str_has_suffix(lower, "+xml")) {
        parse_type = RSVG_XINCLUDE_PARSE_XML;
        goto out;
    }

    if (strcmp(lower, "text/plain") == 0 || g_str_has_prefix(lower, "text/")) {
        parse_type = RSVG_XINCLUDE_PARSE_TEXT;
        goto out;
    }

out:
    g_free(lower);
    g_free(token);
    return parse_type;
}

/* http://www.w3.org/TR/xinclude/ */
static void rsvg_start_xinclude(RsvgHandle* ctx, RsvgPropertyBag* atts) {
    RsvgSaxHandlerXinclude* handler;
    RsvgXincludeParseType parse_type;
    gboolean success = FALSE;
    const char *href, *parse;

    href = rsvg_property_bag_lookup(atts, "href");
    parse = rsvg_property_bag_lookup(atts, "parse");
    parse_type = rsvg_classify_xinclude_parse(parse);

    if (href) {
        /* Security guardrails for XInclude loads:
         * - URI access is restricted in _rsvg_handle_acquire_* via rsvg_allow_load().
         * - nested includes are bounded by RSVG_XINCLUDE_MAX_DEPTH.
         */
        if (parse_type == RSVG_XINCLUDE_PARSE_XML) {
            success = rsvg_load_xinclude_xml(ctx, href);
        }
        else if (parse_type == RSVG_XINCLUDE_PARSE_TEXT) {
            success = rsvg_load_xinclude_text(ctx, href);
        }
    }

    /* needed to handle xi:fallback */
    handler = g_new0(RsvgSaxHandlerXinclude, 1);

    handler->super.parent = ctx->priv->handler;
    handler->super.free = rsvg_xinclude_handler_free;
    handler->super.characters = rsvg_xinclude_handler_characters;
    handler->super.start_element = rsvg_xinclude_handler_start;
    handler->super.end_element = rsvg_xinclude_handler_end;
    handler->ctx = ctx;
    handler->success = success;

    ctx->priv->handler = &handler->super;
}

/* end xinclude */

static gboolean loading_limits_exceeded(RsvgHandle* handle) {
    /* This is a mitigation for SVG files which create millions of elements
     * in an attempt to exhaust memory.  We don't allow loading more than
     * this number of elements during the initial streaming load process.
     */
    if (handle->priv->flags & RSVG_HANDLE_FLAG_UNLIMITED) {
        return FALSE;
    }

    /* For gzip-compressed inputs, use a much lower ceiling; otherwise small
     * `.svgz` "bombs" can take a very long time (and lots of memory) to parse.
     */
    if (handle->priv->is_testing || handle->priv->is_gzipped_input) {
        return handle->priv->num_loaded_elements > 20000;
    }
    return handle->priv->num_loaded_elements > 200000;
}

static void rsvg_start_element(void* data, const xmlChar* name, const xmlChar** atts) {
    RsvgPropertyBag* bag;
    RsvgHandle* ctx = (RsvgHandle*)data;

    /* In a different way from librsvg 2.42, we do the following check here, not
     * in rsvg_standard_element_start() as it is done there.  This is because
     * librsvg 2.40 still creates nodes for <title> and <metadata> elements, and
     * we'd like to prevent unbounded memory consuption for those elements, too.
     */
    if (loading_limits_exceeded(ctx)) {
        g_set_error(ctx->priv->error, RSVG_ERROR, 0, "instancing limit");

        xmlStopParser(ctx->priv->ctxt);
        return;
    }

    ctx->priv->num_loaded_elements += 1;

    bag = rsvg_property_bag_new((const char**)atts);
    rsvg_push_xmlns_frame(ctx, bag);

    if (ctx->priv->handler) {
        ctx->priv->handler_nest++;
        if (ctx->priv->handler->start_element != NULL)
            ctx->priv->handler->start_element(ctx->priv->handler, (const char*)name, bag);
    }
    else {
        const char* local_name = get_localname((const char*)name);

        if (!strcmp(local_name, "style"))
            rsvg_start_style(ctx, bag);
        else if (!strcmp(local_name, "title"))
            rsvg_start_title(ctx);
        else if (!strcmp(local_name, "desc"))
            rsvg_start_desc(ctx);
        else if (!strcmp(local_name, "metadata"))
            rsvg_start_metadata(ctx);
        else if (rsvg_is_xinclude_element(ctx, (const char*)name, "include"))
            rsvg_start_xinclude(ctx, bag);
        else
            rsvg_standard_element_start(ctx, local_name, bag);
    }

    rsvg_property_bag_free(bag);
}

static void rsvg_end_element(void* data, const xmlChar* name) {
    RsvgHandle* ctx = (RsvgHandle*)data;
    const char* local_name = get_localname((const char*)name);

    if (ctx->priv->handler_nest > 0 && ctx->priv->handler != NULL) {
        if (ctx->priv->handler->end_element != NULL)
            ctx->priv->handler->end_element(ctx->priv->handler, (const char*)name);
        ctx->priv->handler_nest--;
    }
    else {
        gboolean closed_root_svg = FALSE;

        if (ctx->priv->handler != NULL) {
            rsvg_sax_handler_pop(ctx, TRUE);
        }

        if (ctx->priv->currentnode != NULL) {
            closed_root_svg =
                (ctx->priv->currentnode == ctx->priv->treebase) && !strcmp(local_name, ctx->priv->currentnode->name);
        }

        if (ctx->priv->currentnode && !strcmp(local_name, ctx->priv->currentnode->name))
            rsvg_pop_def_group(ctx);

        if (closed_root_svg)
            _rsvg_node_svg_apply_atts((RsvgNodeSvg*)ctx->priv->treebase, ctx);
    }

    rsvg_pop_xmlns_frame(ctx);
}

static void _rsvg_node_chars_free(RsvgNode* node) {
    RsvgNodeChars* self = (RsvgNodeChars*)node;
    g_string_free(self->contents, TRUE);
    _rsvg_node_free(node);
}

static RsvgNodeChars* rsvg_new_node_chars(const char* text, int len) {
    RsvgNodeChars* self;

    self = g_new(RsvgNodeChars, 1);
    _rsvg_node_init(&self->super, RSVG_NODE_TYPE_CHARS);

    if (!g_utf8_validate(text, len, NULL)) {
        char* utf8;
        utf8 = rsvg_make_valid_utf8(text, len);
        self->contents = g_string_new(utf8);
        g_free(utf8);
    }
    else {
        self->contents = g_string_new_len(text, len);
    }

    self->super.free = _rsvg_node_chars_free;
    self->super.state->cond_true = FALSE;

    return self;
}

static void rsvg_characters_impl(RsvgHandle* ctx, const xmlChar* ch, int len) {
    RsvgNodeChars* self;

    if (!ch || !len)
        return;

    if (ctx->priv->currentnode) {
        RsvgNodeType type = RSVG_NODE_TYPE(ctx->priv->currentnode);
        if (type == RSVG_NODE_TYPE_TSPAN || type == RSVG_NODE_TYPE_TEXT) {
            guint i;

            /* find the last CHARS node in the text or tspan node, so that we
               can coalesce the text, and thus avoid screwing up the Pango layouts */
            self = NULL;
            for (i = 0; i < ctx->priv->currentnode->children->len; i++) {
                RsvgNode* node = g_ptr_array_index(ctx->priv->currentnode->children, i);
                if (RSVG_NODE_TYPE(node) == RSVG_NODE_TYPE_CHARS) {
                    self = (RsvgNodeChars*)node;
                }
                else if (RSVG_NODE_TYPE(node) == RSVG_NODE_TYPE_TSPAN) {
                    self = NULL;
                }
            }

            if (self != NULL) {
                if (!g_utf8_validate((char*)ch, len, NULL)) {
                    char* utf8;
                    utf8 = rsvg_make_valid_utf8((char*)ch, len);
                    g_string_append(self->contents, utf8);
                    g_free(utf8);
                }
                else {
                    g_string_append_len(self->contents, (char*)ch, len);
                }

                return;
            }
        }
    }

    self = rsvg_new_node_chars((char*)ch, len);

    rsvg_defs_register_memory(ctx->priv->defs, (RsvgNode*)self);
    if (ctx->priv->currentnode)
        rsvg_node_group_pack(ctx->priv->currentnode, (RsvgNode*)self);
}

static void rsvg_characters(void* data, const xmlChar* ch, int len) {
    RsvgHandle* ctx = (RsvgHandle*)data;

    if (ctx->priv->handler && ctx->priv->handler->characters != NULL) {
        ctx->priv->handler->characters(ctx->priv->handler, (const char*)ch, len);
        return;
    }

    rsvg_characters_impl(ctx, ch, len);
}

static gboolean rsvg_entity_is_external(xmlEntityPtr entity) {
    if (!entity)
        return FALSE;

    return entity->etype == XML_EXTERNAL_GENERAL_PARSED_ENTITY ||
           entity->etype == XML_EXTERNAL_GENERAL_UNPARSED_ENTITY || entity->etype == XML_EXTERNAL_PARAMETER_ENTITY;
}

static void rsvg_set_external_entity_error(RsvgHandle* ctx) {
    if (ctx->priv->error && *(ctx->priv->error) == NULL) {
        g_set_error(ctx->priv->error, RSVG_ERROR, RSVG_ERROR_FAILED, "External entities are not allowed");
    }
}

static xmlEntityPtr rsvg_get_entity(void* data, const xmlChar* name) {
    RsvgHandle* ctx = (RsvgHandle*)data;
    xmlEntityPtr entity;

    entity = g_hash_table_lookup(ctx->priv->entities, name);

    if (rsvg_entity_is_external(entity)) {
        rsvg_set_external_entity_error(ctx);
        xmlStopParser(ctx->priv->ctxt);
        return NULL;
    }

    return entity;
}

static void rsvg_entity_decl(void* data,
                             const xmlChar* name,
                             int type,
                             const xmlChar* publicId,
                             const xmlChar* systemId,
                             xmlChar* content) {
    RsvgHandle* ctx = (RsvgHandle*)data;
    GHashTable* entities = ctx->priv->entities;
    xmlEntityPtr entity;
    xmlChar *resolvedSystemId = NULL, *resolvedPublicId = NULL;

    if (systemId)
        resolvedSystemId = xmlBuildRelativeURI(systemId, (xmlChar*)rsvg_handle_get_base_uri(ctx));
    else if (publicId)
        resolvedPublicId = xmlBuildRelativeURI(publicId, (xmlChar*)rsvg_handle_get_base_uri(ctx));

    entity = xmlNewEntity(NULL, name, type, resolvedPublicId, resolvedSystemId, content);

    xmlFree(resolvedPublicId);
    xmlFree(resolvedSystemId);

    g_hash_table_insert(entities, g_strdup((const char*)name), entity);
}

static void rsvg_unparsed_entity_decl(void* ctx,
                                      const xmlChar* name,
                                      const xmlChar* publicId,
                                      const xmlChar* systemId,
                                      const xmlChar* notationName) {
    (void)notationName;
    rsvg_entity_decl(ctx, name, XML_INTERNAL_GENERAL_ENTITY, publicId, systemId, NULL);
}

static xmlEntityPtr rsvg_get_parameter_entity(void* data, const xmlChar* name) {
    RsvgHandle* ctx = (RsvgHandle*)data;
    xmlEntityPtr entity;

    entity = g_hash_table_lookup(ctx->priv->entities, name);

    if (rsvg_entity_is_external(entity)) {
        rsvg_set_external_entity_error(ctx);
        xmlStopParser(ctx->priv->ctxt);
        return NULL;
    }

    return entity;
}

static void rsvg_error_cb(void* data, const char* msg, ...) {
    (void)data;
#ifndef G_ENABLE_DEBUG
    (void)msg;
#endif
#ifdef G_ENABLE_DEBUG
    va_list args;

    va_start(args, msg);
    vfprintf(stderr, msg, args);
    va_end(args);
#endif
}

static void rsvg_processing_instruction(void* ctx, const xmlChar* target, const xmlChar* data) {
    /* http://www.w3.org/TR/xml-stylesheet/ */
    RsvgHandle* handle = (RsvgHandle*)ctx;

    if (!strcmp((const char*)target, "xml-stylesheet")) {
        RsvgPropertyBag* atts;
        char** xml_atts;

        xml_atts = rsvg_css_parse_xml_attribute_string((const char*)data);

        if (xml_atts) {
            const char* value;

            atts = rsvg_property_bag_new((const char**)xml_atts);
            value = rsvg_property_bag_lookup(atts, "alternate");
            if (!value || !value[0] || (strcmp(value, "no") != 0)) {
                value = rsvg_property_bag_lookup(atts, "type");
                if (value && strcmp(value, "text/css") == 0) {
                    value = rsvg_property_bag_lookup(atts, "href");
                    if (value && value[0]) {
                        char* style_data;
                        gsize style_data_len;
                        char* mime_type = NULL;

                        style_data = _rsvg_handle_acquire_data(handle, value, &mime_type, &style_data_len, NULL);
                        if (style_data && mime_type && strcmp(mime_type, "text/css") == 0) {
                            rsvg_parse_cssbuffer(handle, style_data, style_data_len);
                        }

                        g_free(mime_type);
                        g_free(style_data);
                    }
                }
            }

            rsvg_property_bag_free(atts);
            g_strfreev(xml_atts);
        }
    }
}

const RsvgSaxCallbacks* rsvg_get_sax_callbacks(void) {
    static const RsvgSaxCallbacks callbacks = {
        .start_element = rsvg_start_element,
        .end_element = rsvg_end_element,
        .characters = rsvg_characters,
        .processing_instruction = rsvg_processing_instruction,
        .get_entity = rsvg_get_entity,
        .entity_decl = rsvg_entity_decl,
        .unparsed_entity_decl = rsvg_unparsed_entity_decl,
        .get_parameter_entity = rsvg_get_parameter_entity,
        .error = rsvg_error_cb,
        .cdata_block = rsvg_characters,
    };

    return &callbacks;
}

/* http://www.ietf.org/rfc/rfc2396.txt */

static gboolean rsvg_path_is_uri(char const* path) {
    char const* p;

    if (path == NULL)
        return FALSE;

    if (strlen(path) < 4)
        return FALSE;

    if ((path[0] < 'a' || path[0] > 'z') && (path[0] < 'A' || path[0] > 'Z')) {
        return FALSE;
    }

    for (p = &path[1]; (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '+' ||
                       *p == '-' || *p == '.';
         p++)
        ;

    if (strlen(p) < 3)
        return FALSE;

    return (p[0] == ':' && p[1] == '/' && p[2] == '/');
}

static gchar* rsvg_get_base_uri_from_filename(const gchar* filename) {
    gchar* current_dir;
    gchar* absolute_filename;
    gchar* base_uri;

    if (g_path_is_absolute(filename))
        return g_filename_to_uri(filename, NULL, NULL);

    current_dir = g_get_current_dir();
    absolute_filename = g_build_filename(current_dir, filename, NULL);
    base_uri = g_filename_to_uri(absolute_filename, NULL, NULL);
    g_free(absolute_filename);
    g_free(current_dir);

    return base_uri;
}

/**
 * rsvg_handle_set_base_uri:
 * @handle: A #RsvgHandle
 * @base_uri: The base uri
 *
 * Set the base URI for this SVG. This can only be called before rsvg_handle_write()
 * has been called.
 *
 * Since: 2.9
 */
void rsvg_handle_set_base_uri(RsvgHandle* handle, const char* base_uri) {
    g_autofree gchar* uri = NULL;
    g_autoptr(GFile) file = NULL;

    g_return_if_fail(handle != NULL);

    if (base_uri == NULL)
        return;

    if (rsvg_path_is_uri(base_uri))
        uri = g_strdup(base_uri);
    else
        uri = rsvg_get_base_uri_from_filename(base_uri);

    file = g_file_new_for_uri(uri ? uri : "data:");
    rsvg_handle_set_base_gfile(handle, file);
}

static void rsvg_node_snapshot_base_state_recursive(RsvgNode* node) {
    guint i;

    if (!node)
        return;

    if (!node->base_state) {
        node->base_state = g_new(RsvgState, 1);
        rsvg_state_init(node->base_state);
        rsvg_state_clone(node->base_state, node->state);
    }

    if (node->children) {
        for (i = 0; i < node->children->len; i++) {
            RsvgNode* child = g_ptr_array_index(node->children, i);
            rsvg_node_snapshot_base_state_recursive(child);
        }
    }
}

static void rsvg_apply_styles_to_node(RsvgHandle* ctx, RsvgNode* node) {
    if (!node->state)
        return;

    if (node->base_state) {
        rsvg_state_clone(node->state, node->base_state);
    }

    rsvg_css_engine_apply_styles(ctx->priv->css_engine, node, node->state, node->name, node->klass, node->id, NULL);
}

static void rsvg_apply_styles_recursive(RsvgHandle* ctx, RsvgNode* node) {
    guint i;

    if (!node)
        return;

    rsvg_apply_styles_to_node(ctx, node);

    if (node->children) {
        for (i = 0; i < node->children->len; i++) {
            RsvgNode* child = g_ptr_array_index(node->children, i);
            rsvg_apply_styles_recursive(ctx, child);
        }
    }
}

gboolean rsvg_handle_check_loaded(RsvgHandle* handle, GError** error) {
    if (handle->priv->is_disposed) {
        g_set_error_literal(error, RSVG_ERROR, RSVG_ERROR_FAILED, "SVG handle is disposed");
        return FALSE;
    }
    if (handle->priv->state != RSVG_HANDLE_STATE_CLOSED_OK || !handle->priv->treebase) {
        g_set_error_literal(error, RSVG_ERROR, RSVG_ERROR_FAILED, "SVG is not fully loaded");
        return FALSE;
    }
    return TRUE;
}

/**
 * rsvg_handle_set_stylesheet:
 * @handle: a fully loaded #RsvgHandle
 * @css: (array length=css_len): a buffer with CSS data
 * @css_len: length of the @css buffer in bytes
 * @error: (allow-none): a location to store a #GError, or %NULL
 *
 * Sets a CSS stylesheet to use for an SVG document.
 *
 * Returns: %TRUE on success, or %FALSE on error.
 *
 * Since: 2.48
 */
gboolean rsvg_handle_set_stylesheet(RsvgHandle* handle, const guint8* css, gsize css_len, GError** error) {
    g_return_val_if_fail(handle != NULL, FALSE);
    g_return_val_if_fail(css != NULL, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

    if (!rsvg_handle_check_loaded(handle, error))
        return FALSE;

    if (!rsvg_css_engine_set_user_stylesheet(handle->priv->css_engine, (const char*)css, (size_t)css_len)) {
        g_set_error_literal(error, RSVG_ERROR, RSVG_ERROR_FAILED, _("Failed to parse CSS stylesheet"));
        return FALSE;
    }

    rsvg_node_snapshot_base_state_recursive(handle->priv->treebase);
    rsvg_apply_styles_recursive(handle, handle->priv->treebase);

    return TRUE;
}

/**
 * rsvg_handle_set_base_gfile:
 * @handle: a #RsvgHandle
 * @base_file: a #GFile
 *
 * Set the base URI for @handle from @file.
 * Note: This function may only be called before rsvg_handle_write()
 * or rsvg_handle_read_stream_sync() has been called.
 *
 * Since: 2.32
 */
void rsvg_handle_set_base_gfile(RsvgHandle* handle, GFile* base_file) {
    RsvgHandlePrivate* priv;

    g_return_if_fail(RSVG_IS_HANDLE(handle));
    g_return_if_fail(G_IS_FILE(base_file));

    priv = handle->priv;

    g_object_ref(base_file);
    if (priv->base_gfile)
        g_object_unref(priv->base_gfile);
    priv->base_gfile = base_file;

    g_free(priv->base_uri);
    priv->base_uri = g_file_get_uri(base_file);
}

/**
 * rsvg_handle_get_base_uri:
 * @handle: A #RsvgHandle
 *
 * Gets the base uri for this #RsvgHandle.
 *
 * Returns: the base uri, possibly null
 * Since: 2.8
 */
const char* rsvg_handle_get_base_uri(RsvgHandle* handle) {
    g_return_val_if_fail(handle, NULL);
    return handle->priv->base_uri;
}

/**
 * rsvg_error_quark:
 *
 * The error domain for RSVG
 *
 * Returns: The error domain
 */
GQuark rsvg_error_quark(void) {
    /* don't use from_static_string(), since librsvg might be used in a module
       that's ultimately unloaded */
    return g_quark_from_string("rsvg-error-quark");
}

static gboolean rsvg_handle_write_impl(RsvgHandle* handle, const guchar* buf, gsize count, GError** error) {
    GError* real_error = NULL;
    int result;

    rsvg_return_val_if_fail(handle != NULL, FALSE, error);

    handle->priv->error = &real_error;

    if (handle->priv->ctxt == NULL) {
        handle->priv->ctxt = create_xml_push_parser(handle, rsvg_handle_get_base_uri(handle));
    }

    do {
        int chunk = MIN(count, (gsize)G_MAXINT);

        result = xmlParseChunk(handle->priv->ctxt, (const char*)buf, chunk, 0);
        if (result != 0 || real_error != NULL)
            break;
        count -= chunk;
        buf += chunk;
    } while (count > 0);

    handle->priv->error = NULL;

    if (result != 0 || real_error != NULL) {
        handle->priv->state = RSVG_HANDLE_STATE_CLOSED_ERROR;
        if (real_error != NULL)
            g_propagate_error(error, real_error);
        else
            rsvg_xml_set_error(error, handle->priv->ctxt);
        return FALSE;
    }

    return TRUE;
}

static gboolean rsvg_handle_close_impl(RsvgHandle* handle, GError** error) {
    GError* real_error = NULL;

    handle->priv->error = &real_error;

    if (handle->priv->ctxt != NULL) {
        int result;

        result = xmlParseChunk(handle->priv->ctxt, "", 0, TRUE);
        handle->priv->error = NULL;
        if (result != 0) {
            if (real_error != NULL)
                g_propagate_error(error, real_error);
            else
                rsvg_xml_set_error(error, handle->priv->ctxt);
            handle->priv->ctxt = rsvg_free_xml_parser_and_doc(handle->priv->ctxt);
            return FALSE;
        }

        handle->priv->ctxt = rsvg_free_xml_parser_and_doc(handle->priv->ctxt);
    }

    handle->priv->error = NULL;

    if (real_error != NULL) {
        g_propagate_error(error, real_error);
        return FALSE;
    }

    return TRUE;
}

void rsvg_drawing_ctx_free(RsvgDrawingCtx* handle) {
    if (handle->pango_context != NULL)
        g_object_unref(handle->pango_context);

    rsvg_render_free(handle->render);

    rsvg_state_free_all(handle->state);

    /* the drawsub stack's nodes are owned by the ->defs */
    g_slist_free(handle->drawsub_stack);

    g_warn_if_fail(handle->acquired_nodes == NULL);
    g_slist_free(handle->acquired_nodes);

    g_free(handle);
}

/**
 * rsvg_handle_get_metadata:
 * @handle: An #RsvgHandle
 *
 * Returns the SVG's metadata in UTF-8 or %NULL. You must make a copy
 * of this metadata if you wish to use it after @handle has been freed.
 *
 * Returns: (nullable): The SVG's title
 *
 * Since: 2.9
 *
 * Deprecated: 2.36
 */
const char* rsvg_handle_get_metadata(RsvgHandle* handle) {
    g_return_val_if_fail(handle, NULL);

    if (handle->priv->metadata)
        return handle->priv->metadata->str;
    else
        return NULL;
}

/**
 * rsvg_handle_get_title:
 * @handle: An #RsvgHandle
 *
 * Returns the SVG's title in UTF-8 or %NULL. You must make a copy
 * of this title if you wish to use it after @handle has been freed.
 *
 * Returns: (nullable): The SVG's title
 *
 * Since: 2.4
 *
 * Deprecated: 2.36
 */
const char* rsvg_handle_get_title(RsvgHandle* handle) {
    g_return_val_if_fail(handle, NULL);

    if (handle->priv->title)
        return handle->priv->title->str;
    else
        return NULL;
}

/**
 * rsvg_handle_get_desc:
 * @handle: An #RsvgHandle
 *
 * Returns the SVG's description in UTF-8 or %NULL. You must make a copy
 * of this description if you wish to use it after @handle has been freed.
 *
 * Returns: (nullable): The SVG's description
 *
 * Since: 2.4
 *
 * Deprecated: 2.36
 */
const char* rsvg_handle_get_desc(RsvgHandle* handle) {
    g_return_val_if_fail(handle, NULL);

    if (handle->priv->desc)
        return handle->priv->desc->str;
    else
        return NULL;
}

/**
 * rsvg_handle_get_dimensions:
 * @handle: A #RsvgHandle
 * @dimension_data: (out): A place to store the SVG's size
 *
 * Get the SVG's size. Do not call from within the size_func callback, because an infinite loop will occur.
 *
 * Since: 2.14
 */
void rsvg_handle_get_dimensions(RsvgHandle* handle, RsvgDimensionData* dimension_data) {
    /* This function is probably called from the cairo_render functions.
     * To prevent an infinite loop we are saving the state.
     */
    if (!handle->priv->in_loop) {
        handle->priv->in_loop = TRUE;
        rsvg_handle_get_dimensions_sub(handle, dimension_data, NULL);
        handle->priv->in_loop = FALSE;
    }
    else {
        /* Called within the size function, so return a standard size */
        dimension_data->em = dimension_data->width = 1;
        dimension_data->ex = dimension_data->height = 1;
    }
}

/**
 * rsvg_handle_get_intrinsic_size_in_pixels:
 * @handle: A #RsvgHandle
 * @out_width: (out) (optional): where to store the width in pixels
 * @out_height: (out) (optional): where to store the height in pixels
 *
 * Returns the intrinsic size for the SVG, if its units can be resolved to
 * pixels. Font-relative units use the root element's font size; percentages
 * require a viewport and cannot be resolved here.
 *
 * Returns: %TRUE if a pixel size can be computed
 */
gboolean rsvg_handle_get_intrinsic_size_in_pixels(RsvgHandle* handle, gdouble* out_width, gdouble* out_height) {
    RsvgNodeSvg* root;
    gdouble width;
    gdouble height;

    g_return_val_if_fail(handle != NULL, FALSE);

    root = (RsvgNodeSvg*)handle->priv->treebase;
    if (!root)
        return FALSE;

    if (root->w.length == -1 || root->h.length == -1)
        return FALSE;

    if (root->w.factor == 'p' || root->h.factor == 'p')
        return FALSE;

    if (root->w.factor == 'l' || root->w.factor == 's')
        return FALSE;

    if (root->h.factor == 'l' || root->h.factor == 's')
        return FALSE;

    double font_size = 12;
    if (root->w.factor == 'm' || root->w.factor == 'x' || root->h.factor == 'm' || root->h.factor == 'x') {
        RsvgDrawingCtx ctx = {.dpi_x = handle->priv->dpi_x, .dpi_y = handle->priv->dpi_y};
        font_size = _rsvg_css_normalize_font_size(root->super.state, &ctx);
    }
    width = _rsvg_css_hand_normalize_length(&root->w, handle->priv->dpi_x, 0, font_size);
    height = _rsvg_css_hand_normalize_length(&root->h, handle->priv->dpi_y, 0, font_size);

    if (out_width)
        *out_width = width;
    if (out_height)
        *out_height = height;

    return TRUE;
}

static RsvgLengthUnit rsvg_length_unit_from_factor(char factor) {
    switch (factor) {
        case 'p':
            return RSVG_LENGTH_UNIT_PERCENT;
        case 'm':
            return RSVG_LENGTH_UNIT_EM;
        case 'x':
            return RSVG_LENGTH_UNIT_EX;
        case 'i':
            return RSVG_LENGTH_UNIT_IN;
        case 'l':
        case 's':
        default:
            return RSVG_LENGTH_UNIT_PX;
    }
}

/**
 * rsvg_handle_get_intrinsic_dimensions:
 * @handle: A #RsvgHandle
 * @out_has_width: (out) (optional): whether the width length is available
 * @out_width: (out caller-allocates) (optional): the intrinsic width length
 * @out_has_height: (out) (optional): whether the height length is available
 * @out_height: (out caller-allocates) (optional): the intrinsic height length
 * @out_has_viewbox: (out) (optional): whether a viewBox is present
 * @out_viewbox: (out caller-allocates) (optional): the viewBox rectangle
 *
 * Reports the root SVG's lengths and optional viewBox without converting
 * lengths to pixels. Unspecified width and height default to 100%.
 * Read each length or rectangle only when its corresponding flag is true.
 * Use rsvg_handle_get_intrinsic_size_in_pixels() for resolved pixel sizes.
 */
void rsvg_handle_get_intrinsic_dimensions(RsvgHandle* handle,
                                          gboolean* out_has_width,
                                          RsvgLength* out_width,
                                          gboolean* out_has_height,
                                          RsvgLength* out_height,
                                          gboolean* out_has_viewbox,
                                          RsvgRectangle* out_viewbox) {
    RsvgNodeSvg* root;

    g_return_if_fail(handle != NULL);

    root = (RsvgNodeSvg*)handle->priv->treebase;
    if (!root) {
        if (out_has_width)
            *out_has_width = FALSE;
        if (out_has_height)
            *out_has_height = FALSE;
        if (out_has_viewbox)
            *out_has_viewbox = FALSE;
        return;
    }

    if (out_has_width)
        *out_has_width = (root->w.length != -1);
    if (out_has_height)
        *out_has_height = (root->h.length != -1);
    if (out_has_viewbox)
        *out_has_viewbox = root->vbox.active;

    if (out_width) {
        out_width->length = root->w.length;
        out_width->factor = root->w.factor;
        out_width->unit = rsvg_length_unit_from_factor(root->w.factor);
    }

    if (out_height) {
        out_height->length = root->h.length;
        out_height->factor = root->h.factor;
        out_height->unit = rsvg_length_unit_from_factor(root->h.factor);
    }

    if (out_viewbox) {
        if (root->vbox.active) {
            out_viewbox->x = root->vbox.rect.x;
            out_viewbox->y = root->vbox.rect.y;
            out_viewbox->width = root->vbox.rect.width;
            out_viewbox->height = root->vbox.rect.height;
        }
        else {
            out_viewbox->x = 0.0;
            out_viewbox->y = 0.0;
            out_viewbox->width = 0.0;
            out_viewbox->height = 0.0;
        }
    }
}

/**
 * rsvg_handle_get_dimensions_sub:
 * @handle: A #RsvgHandle
 * @dimension_data: (out): A place to store the SVG's size
 * @id: (nullable): An element's id within the SVG, or %NULL to get
 *   the dimension of the whole SVG.  For example, if you have a layer
 *   called "layer1" for that you want to get the dimension, pass
 *   "#layer1" as the id.
 *
 * Get the size of a subelement of the SVG file. Do not call from within the size_func callback, because an infinite
 * loop will occur.
 *
 * Since: 2.22
 */
gboolean rsvg_handle_get_dimensions_sub(RsvgHandle* handle, RsvgDimensionData* dimension_data, const char* id) {
    cairo_t* cr;
    cairo_surface_t* target;
    RsvgDrawingCtx* draw;
    RsvgNodeSvg* root = NULL;
    RsvgNode* sself = NULL;
    RsvgBbox bbox;
    gboolean retval = FALSE;

    gboolean handle_subelement = TRUE;

    g_return_val_if_fail(handle, FALSE);
    g_return_val_if_fail(dimension_data, FALSE);

    memset(dimension_data, 0, sizeof(RsvgDimensionData));

    if (id && *id) {
        sself = rsvg_defs_lookup(handle->priv->defs, id);

        if (sself == handle->priv->treebase)
            id = NULL;
    }
    else
        sself = handle->priv->treebase;

    if (!sself && id)
        return FALSE;

    root = (RsvgNodeSvg*)handle->priv->treebase;

    if (!root)
        return FALSE;

    bbox.rect.x = bbox.rect.y = 0;
    bbox.rect.width = bbox.rect.height = 1;

    if (!id && (root->w.factor == 'p' || root->h.factor == 'p') && !root->vbox.active)
        handle_subelement = TRUE;
    else if (!id && root->w.length != -1 && root->h.length != -1)
        handle_subelement = FALSE;

    if (handle_subelement == TRUE) {
        target = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 1, 1);
        cr = cairo_create(target);

        draw = rsvg_cairo_new_drawing_ctx(cr, handle);

        if (!draw) {
            cairo_destroy(cr);
            cairo_surface_destroy(target);

            return FALSE;
        }

        while (sself != NULL) {
            draw->drawsub_stack = g_slist_prepend(draw->drawsub_stack, sself);
            sself = sself->parent;
        }

        rsvg_state_push(draw);
        cairo_save(cr);

        rsvg_node_draw(handle->priv->treebase, draw, 0);
        bbox = RSVG_CAIRO_RENDER(draw->render)->bounds.ink;

        if (rsvg_drawing_ctx_limits_exceeded(draw)) {
            retval = FALSE;
        }
        else {
            retval = TRUE;
        }

        cairo_restore(cr);
        rsvg_state_pop(draw);
        rsvg_drawing_ctx_free(draw);
        cairo_destroy(cr);
        cairo_surface_destroy(target);

        dimension_data->width = bbox.rect.width;
        dimension_data->height = bbox.rect.height;
    }
    else {
        bbox.rect.width = root->vbox.rect.width;
        bbox.rect.height = root->vbox.rect.height;

        dimension_data->width = (int)(_rsvg_css_hand_normalize_length(&root->w, handle->priv->dpi_x,
                                                                      bbox.rect.width + bbox.rect.x * 2, 12) +
                                      0.5);
        dimension_data->height = (int)(_rsvg_css_hand_normalize_length(&root->h, handle->priv->dpi_y,
                                                                       bbox.rect.height + bbox.rect.y * 2, 12) +
                                       0.5);

        retval = TRUE;
    }

    dimension_data->em = dimension_data->width;
    dimension_data->ex = dimension_data->height;

    if (handle->priv->size_func)
        (*handle->priv->size_func)(&dimension_data->width, &dimension_data->height, handle->priv->user_data);

    return retval;
}

/**
 * rsvg_handle_get_position_sub:
 * @handle: A #RsvgHandle
 * @position_data: (out): A place to store the SVG fragment's position.
 * @id: An element's id within the SVG.
 * For example, if you have a layer called "layer1" for that you want to get
 * the position, pass "##layer1" as the id.
 *
 * Get the position of a subelement of the SVG file. Do not call from within
 * the size_func callback, because an infinite loop will occur.
 *
 * Since: 2.22
 */
gboolean rsvg_handle_get_position_sub(RsvgHandle* handle, RsvgPositionData* position_data, const char* id) {
    RsvgDrawingCtx* draw;
    RsvgNodeSvg* root;
    RsvgNode* node;
    RsvgBbox bbox;
    RsvgDimensionData dimension_data;
    cairo_surface_t* target = NULL;
    cairo_t* cr = NULL;
    gboolean retval = FALSE;

    g_return_val_if_fail(handle, FALSE);
    g_return_val_if_fail(position_data, FALSE);

    /* Short-cut when no id is given. */
    if (NULL == id || '\0' == *id) {
        position_data->x = 0;
        position_data->y = 0;
        return TRUE;
    }

    memset(position_data, 0, sizeof(*position_data));
    memset(&dimension_data, 0, sizeof(dimension_data));

    node = rsvg_defs_lookup(handle->priv->defs, id);
    if (!node) {
        return FALSE;
    }
    else if (node == handle->priv->treebase) {
        /* Root node. */
        position_data->x = 0;
        position_data->y = 0;
        return TRUE;
    }

    root = (RsvgNodeSvg*)handle->priv->treebase;
    if (!root)
        return FALSE;

    target = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 1, 1);
    cr = cairo_create(target);
    draw = rsvg_cairo_new_drawing_ctx(cr, handle);
    if (!draw)
        goto bail;

    while (node != NULL) {
        draw->drawsub_stack = g_slist_prepend(draw->drawsub_stack, node);
        node = node->parent;
    }

    rsvg_state_push(draw);
    cairo_save(cr);

    rsvg_node_draw(handle->priv->treebase, draw, 0);
    bbox = RSVG_CAIRO_RENDER(draw->render)->bounds.ink;

    if (rsvg_drawing_ctx_limits_exceeded(draw)) {
        retval = FALSE;
    }
    else {
        retval = TRUE;
    }

    cairo_restore(cr);
    rsvg_state_pop(draw);
    rsvg_drawing_ctx_free(draw);

    position_data->x = bbox.rect.x;
    position_data->y = bbox.rect.y;
    dimension_data.width = bbox.rect.width;
    dimension_data.height = bbox.rect.height;

    dimension_data.em = dimension_data.width;
    dimension_data.ex = dimension_data.height;

    if (handle->priv->size_func)
        (*handle->priv->size_func)(&dimension_data.width, &dimension_data.height, handle->priv->user_data);

bail:
    if (cr)
        cairo_destroy(cr);
    if (target)
        cairo_surface_destroy(target);

    return retval;
}

/**
 * rsvg_handle_has_sub:
 * @handle: a #RsvgHandle
 * @id: an element's id within the SVG
 *
 * Checks whether the element @id exists in the SVG document.
 *
 * Returns: %TRUE if @id exists in the SVG document
 *
 * Since: 2.22
 */
gboolean rsvg_handle_has_sub(RsvgHandle* handle, const char* id) {
    g_return_val_if_fail(handle, FALSE);

    if (G_UNLIKELY(!id || !id[0]))
        return FALSE;

    return rsvg_defs_lookup(handle->priv->defs, id) != NULL;
}

/**
 * rsvg_set_default_dpi:
 * @dpi: Dots Per Inch (aka Pixels Per Inch)
 *
 * Sets the DPI for the all future outgoing pixbufs. Common values are
 * 75, 90, and 300 DPI. Passing a number <= 0 to @dpi will
 * reset the DPI to whatever the default value happens to be.
 *
 * Since: 2.8
 */
void rsvg_set_default_dpi(double dpi) {
    rsvg_set_default_dpi_x_y(dpi, dpi);
}

/**
 * rsvg_set_default_dpi_x_y:
 * @dpi_x: Dots Per Inch (aka Pixels Per Inch)
 * @dpi_y: Dots Per Inch (aka Pixels Per Inch)
 *
 * Sets the DPI for the all future outgoing pixbufs. Common values are
 * 75, 90, and 300 DPI. Passing a number <= 0 to @dpi will
 * reset the DPI to whatever the default value happens to be.
 *
 * Since: 2.8
 */
void rsvg_set_default_dpi_x_y(double dpi_x, double dpi_y) {
    if (dpi_x <= 0.)
        rsvg_internal_dpi_x = RSVG_DEFAULT_DPI_X;
    else
        rsvg_internal_dpi_x = dpi_x;

    if (dpi_y <= 0.)
        rsvg_internal_dpi_y = RSVG_DEFAULT_DPI_Y;
    else
        rsvg_internal_dpi_y = dpi_y;
}

/**
 * rsvg_handle_set_dpi:
 * @handle: An #RsvgHandle
 * @dpi: Dots Per Inch (aka Pixels Per Inch)
 *
 * Sets the DPI for the outgoing pixbuf. Common values are
 * 75, 90, and 300 DPI. Passing a number <= 0 to @dpi will
 * reset the DPI to whatever the default value happens to be.
 *
 * Since: 2.8
 */
void rsvg_handle_set_dpi(RsvgHandle* handle, double dpi) {
    rsvg_handle_set_dpi_x_y(handle, dpi, dpi);
}

/**
 * rsvg_handle_set_dpi_x_y:
 * @handle: An #RsvgHandle
 * @dpi_x: Dots Per Inch (aka Pixels Per Inch)
 * @dpi_y: Dots Per Inch (aka Pixels Per Inch)
 *
 * Sets the DPI for the outgoing pixbuf. Common values are
 * 75, 90, and 300 DPI. Passing a number <= 0 to #dpi_x or @dpi_y will
 * reset the DPI to whatever the default value happens to be.
 *
 * Since: 2.8
 */
void rsvg_handle_set_dpi_x_y(RsvgHandle* handle, double dpi_x, double dpi_y) {
    g_return_if_fail(handle != NULL);

    if (dpi_x <= 0.)
        handle->priv->dpi_x = rsvg_internal_dpi_x;
    else
        handle->priv->dpi_x = dpi_x;

    if (dpi_y <= 0.)
        handle->priv->dpi_y = rsvg_internal_dpi_y;
    else
        handle->priv->dpi_y = dpi_y;
}

/**
 * rsvg_handle_set_size_callback:
 * @handle: An #RsvgHandle
 * @size_func: (nullable): A sizing function, or %NULL
 * @user_data: User data to pass to @size_func, or %NULL
 * @user_data_destroy: Destroy function for @user_data, or %NULL
 *
 * Sets the sizing function for the @handle.  This function is called right
 * after the size of the image has been loaded.  The size of the image is passed
 * in to the function, which may then modify these values to set the real size
 * of the generated pixbuf.  If the image has no associated size, then the size
 * arguments are set to -1.
 *
 * During or after handle disposal, no sizing function is retained. If
 * @user_data_destroy is provided, it is called immediately for @user_data,
 * even when @size_func is %NULL.
 *
 * Deprecated: Set up a cairo matrix and use rsvg_handle_render_cairo() instead.
 * You can call rsvg_handle_get_dimensions() to figure out the size of your SVG,
 * and then scale it to the desired size via Cairo.  For example, the following
 * code renders an SVG at a specified size, scaled proportionally from whatever
 * original size it may have had:
 *
 * |[<!-- language="C" -->
 * void
 * render_scaled_proportionally (RsvgHandle *handle, cairo_t cr, int width, int height)
 * {
 *     RsvgDimensionData dimensions;
 *     double x_factor, y_factor;
 *     double scale_factor;
 *
 *     rsvg_handle_get_dimensions (handle, &dimensions);
 *
 *     x_factor = (double) width / dimensions.width;
 *     y_factor = (double) height / dimensions.height;
 *
 *     scale_factor = MIN (x_factor, y_factor);
 *
 *     cairo_scale (cr, scale_factor, scale_factor);
 *
 *     rsvg_handle_render_cairo (handle, cr);
 * }
 * ]|
 **/
void rsvg_handle_set_size_callback(RsvgHandle* handle,
                                   RsvgSizeFunc size_func,
                                   gpointer user_data,
                                   GDestroyNotify user_data_destroy) {
    g_return_if_fail(handle != NULL);

    if (handle->priv->is_disposed) {
        if (user_data_destroy)
            user_data_destroy(user_data);
        return;
    }

    GDestroyNotify old_destroy = handle->priv->user_data_destroy;
    gpointer old_data = handle->priv->user_data;

    handle->priv->size_func = size_func;
    handle->priv->user_data = user_data;
    handle->priv->user_data_destroy = user_data_destroy;

    if (old_destroy)
        old_destroy(old_data);
}

#define GZ_MAGIC_0 ((guchar)0x1f)
#define GZ_MAGIC_1 ((guchar)0x8b)

static gboolean rsvg_handle_write_compressed_chunk(RsvgHandle* handle,
                                                   const guchar* buf,
                                                   gsize count,
                                                   GConverterFlags flags,
                                                   GError** error) {
    RsvgHandlePrivate* priv = handle->priv;
    gsize input_offset = 0;

    g_assert(priv->compressed_decompressor != NULL);

    while (TRUE) {
        guchar output_buf[8192];
        gsize bytes_read = 0;
        gsize bytes_written = 0;
        const guchar* input_buf = NULL;
        gsize input_size = 0;
        GConverterResult result;

        if (input_offset < count) {
            input_buf = buf + input_offset;
            input_size = count - input_offset;
        }

        result = g_converter_convert(G_CONVERTER(priv->compressed_decompressor), input_buf, input_size, output_buf,
                                     sizeof(output_buf), flags, &bytes_read, &bytes_written, error);

        input_offset += bytes_read;

        if (bytes_written > 0) {
            if (!rsvg_handle_write_impl(handle, output_buf, bytes_written, error))
                return FALSE;
        }

        if (result == G_CONVERTER_ERROR) {
            return FALSE;
        }

        if (result == G_CONVERTER_FINISHED) {
            if (input_offset < count) {
                g_set_error_literal(error, RSVG_ERROR, RSVG_ERROR_FAILED, "Compressed SVG stream has trailing bytes");
                return FALSE;
            }

            return TRUE;
        }

        if (input_offset == count && !(flags & G_CONVERTER_INPUT_AT_END)) {
            return TRUE;
        }

        if (bytes_read == 0 && bytes_written == 0) {
            g_set_error_literal(error, RSVG_ERROR, RSVG_ERROR_FAILED, "Invalid compressed SVG stream");
            return FALSE;
        }
    }
}

/* Creates handle->priv->compressed_decompressor and primes it with the gzip
 * header data. We consume the header bytes in rsvg_handle_write(), so we need
 * to push them here.
 */
static gboolean create_compressed_decompressor(RsvgHandle* handle, GError** error) {
    RsvgHandlePrivate* priv = handle->priv;
    static const guchar gz_magic[2] = {GZ_MAGIC_0, GZ_MAGIC_1};

    g_assert(priv->compressed_decompressor == NULL);

    priv->compressed_decompressor = g_zlib_decompressor_new(G_ZLIB_COMPRESSOR_FORMAT_GZIP);

    return rsvg_handle_write_compressed_chunk(handle, gz_magic, sizeof(gz_magic), G_CONVERTER_NO_FLAGS, error);
}

/**
 * rsvg_handle_write:
 * @handle: an #RsvgHandle
 * @buf: (array length=count) (element-type guchar): pointer to svg data
 * @count: length of the @buf buffer in bytes
 * @error: (allow-none): a location to store a #GError, or %NULL
 *
 * Loads the next @count bytes of the image.  This will return %TRUE if the data
 * was loaded successful, and %FALSE if an error occurred.  In the latter case,
 * the loader will be closed, and will not accept further writes. If %FALSE is
 * returned, @error will be set to an error from the #RsvgError domain. Errors
 * from #GIOErrorEnum are also possible.
 *
 * Returns: %TRUE on success, or %FALSE on error
 **/
gboolean rsvg_handle_write(RsvgHandle* handle, const guchar* buf, gsize count, GError** error) {
    RsvgHandlePrivate* priv;

    rsvg_return_val_if_fail(handle, FALSE, error);
    priv = handle->priv;

    rsvg_return_val_if_fail(priv->state == RSVG_HANDLE_STATE_START || priv->state == RSVG_HANDLE_STATE_EXPECTING_GZ_1 ||
                                priv->state == RSVG_HANDLE_STATE_READING_COMPRESSED ||
                                priv->state == RSVG_HANDLE_STATE_READING,
                            FALSE, error);

    while (count > 0) {
        switch (priv->state) {
            case RSVG_HANDLE_STATE_START:
                if (buf[0] == GZ_MAGIC_0) {
                    priv->state = RSVG_HANDLE_STATE_EXPECTING_GZ_1;
                    buf++;
                    count--;
                }
                else {
                    priv->state = RSVG_HANDLE_STATE_READING;
                    return rsvg_handle_write_impl(handle, buf, count, error);
                }

                break;

            case RSVG_HANDLE_STATE_EXPECTING_GZ_1:
                if (buf[0] == GZ_MAGIC_1) {
                    priv->state = RSVG_HANDLE_STATE_READING_COMPRESSED;
                    priv->is_gzipped_input = TRUE;
                    if (!create_compressed_decompressor(handle, error)) {
                        if (priv->compressed_decompressor) {
                            g_object_unref(priv->compressed_decompressor);
                            priv->compressed_decompressor = NULL;
                        }
                        priv->state = RSVG_HANDLE_STATE_CLOSED_ERROR;
                        return FALSE;
                    }
                    buf++;
                    count--;
                }
                else {
                    static const guchar gz_magic_0[1] = {GZ_MAGIC_0};
                    priv->state = RSVG_HANDLE_STATE_READING;
                    if (!rsvg_handle_write_impl(handle, gz_magic_0, sizeof(gz_magic_0), error)) {
                        priv->state = RSVG_HANDLE_STATE_CLOSED_ERROR;
                        return FALSE;
                    }
                    return rsvg_handle_write_impl(handle, buf, count, error);
                }

                break;

            case RSVG_HANDLE_STATE_READING_COMPRESSED:
                if (!rsvg_handle_write_compressed_chunk(handle, buf, count, G_CONVERTER_NO_FLAGS, error)) {
                    if (priv->compressed_decompressor) {
                        g_object_unref(priv->compressed_decompressor);
                        priv->compressed_decompressor = NULL;
                    }
                    priv->state = RSVG_HANDLE_STATE_CLOSED_ERROR;
                    return FALSE;
                }
                return TRUE;

            case RSVG_HANDLE_STATE_READING:
                return rsvg_handle_write_impl(handle, buf, count, error);

            default:
                g_assert_not_reached();
        }
    }

    return TRUE;
}

/**
 * rsvg_handle_close:
 * @handle: a #RsvgHandle
 * @error: (allow-none): a location to store a #GError, or %NULL
 *
 * Closes @handle, to indicate that loading the image is complete.  This will
 * return %TRUE if the loader closed successfully.  Note that @handle isn't
 * freed until @g_object_unref is called.
 *
 * Returns: %TRUE on success, or %FALSE on error
 **/
gboolean rsvg_handle_close(RsvgHandle* handle, GError** error) {
    RsvgHandlePrivate* priv;
    gboolean result;

    rsvg_return_val_if_fail(handle, FALSE, error);
    priv = handle->priv;

    if (priv->state == RSVG_HANDLE_STATE_CLOSED_OK) {
        /* closing is idempotent */
        return TRUE;
    }

    if (priv->state == RSVG_HANDLE_STATE_CLOSED_ERROR) {
        g_set_error(error, RSVG_ERROR, RSVG_ERROR_FAILED, "Handle is already closed with an error");
        return FALSE;
    }

    if (priv->state == RSVG_HANDLE_STATE_EXPECTING_GZ_1) {
        static const guchar gz_magic_0[1] = {GZ_MAGIC_0};
        priv->state = RSVG_HANDLE_STATE_READING;
        if (!rsvg_handle_write_impl(handle, gz_magic_0, sizeof(gz_magic_0), error)) {
            priv->state = RSVG_HANDLE_STATE_CLOSED_ERROR;
            return FALSE;
        }
    }

    if (priv->state == RSVG_HANDLE_STATE_READING_COMPRESSED) {
        if (!rsvg_handle_write_compressed_chunk(handle, NULL, 0, G_CONVERTER_INPUT_AT_END, error)) {
            if (priv->compressed_decompressor) {
                g_object_unref(priv->compressed_decompressor);
                priv->compressed_decompressor = NULL;
            }
            priv->state = RSVG_HANDLE_STATE_CLOSED_ERROR;
            return FALSE;
        }

        g_object_unref(priv->compressed_decompressor);
        priv->compressed_decompressor = NULL;

        result = rsvg_handle_close_impl(handle, error);

        if (result) {
            priv->state = RSVG_HANDLE_STATE_CLOSED_OK;
        }
        else {
            priv->state = RSVG_HANDLE_STATE_CLOSED_ERROR;
        }

        return result;
    }

    result = rsvg_handle_close_impl(handle, error);

    if (result) {
        priv->state = RSVG_HANDLE_STATE_CLOSED_OK;
    }
    else {
        priv->state = RSVG_HANDLE_STATE_CLOSED_ERROR;
    }

    return result;
}

/**
 * rsvg_handle_read_stream_sync:
 * @handle: a #RsvgHandle
 * @stream: a #GInputStream
 * @cancellable: (allow-none): a #GCancellable, or %NULL
 * @error: (allow-none): a location to store a #GError, or %NULL
 *
 * Reads @stream and writes the data from it to @handle.
 *
 * If @cancellable is not %NULL, then the operation can be cancelled by
 * triggering the cancellable object from another thread. If the
 * operation was cancelled, the error %G_IO_ERROR_CANCELLED will be
 * returned.
 *
 * Returns: %TRUE if reading @stream succeeded, or %FALSE otherwise
 *   with @error filled in
 *
 * Since: 2.32
 */
gboolean rsvg_handle_read_stream_sync(RsvgHandle* handle,
                                      GInputStream* stream,
                                      GCancellable* cancellable,
                                      GError** error) {
    RsvgHandlePrivate* priv;
    int result;
    g_autoptr(GError) err = NULL;
    gboolean res = FALSE;
    const guchar* buf;
    gssize num_read;
    g_autoptr(GInputStream) buffered_stream = NULL;

    g_return_val_if_fail(RSVG_IS_HANDLE(handle), FALSE);
    g_return_val_if_fail(G_IS_INPUT_STREAM(stream), FALSE);
    g_return_val_if_fail(cancellable == NULL || G_IS_CANCELLABLE(cancellable), FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

    priv = handle->priv;

    g_return_val_if_fail(priv->state == RSVG_HANDLE_STATE_START, FALSE);

    if (g_cancellable_set_error_if_cancelled(cancellable, error)) {
        priv->state = RSVG_HANDLE_STATE_CLOSED_ERROR;
        return FALSE;
    }

    /* detect zipped streams */
    buffered_stream = g_buffered_input_stream_new(stream);
    num_read = g_buffered_input_stream_fill(G_BUFFERED_INPUT_STREAM(buffered_stream), 2, cancellable, error);
    if (num_read < 2) {
        priv->state = RSVG_HANDLE_STATE_CLOSED_ERROR;
        if (num_read >= 0) {
            g_set_error(error, rsvg_error_quark(), RSVG_ERROR_FAILED, _("Input file is too short"));
        }

        return FALSE;
    }

    buf = g_buffered_input_stream_peek_buffer(G_BUFFERED_INPUT_STREAM(buffered_stream), NULL);
    priv->is_gzipped_input = ((buf[0] == GZ_MAGIC_0) && (buf[1] == GZ_MAGIC_1));
    if (priv->is_gzipped_input) {
        g_autoptr(GConverter) converter = G_CONVERTER(g_zlib_decompressor_new(G_ZLIB_COMPRESSOR_FORMAT_GZIP));
        GInputStream* conv_stream = g_converter_input_stream_new(buffered_stream, converter);
        g_clear_object(&buffered_stream);
        buffered_stream = conv_stream;
    }

    priv->error = &err;
    priv->cancellable = cancellable ? g_object_ref(cancellable) : NULL;

    g_assert(handle->priv->ctxt == NULL);
    handle->priv->ctxt = create_xml_stream_parser(handle, buffered_stream, cancellable, &err);

    if (!handle->priv->ctxt) {
        if (err) {
            g_propagate_error(error, g_steal_pointer(&err));
        }

        goto out;
    }

    result = xmlParseDocument(priv->ctxt);
    if (result != 0) {
        if (err)
            g_propagate_error(error, g_steal_pointer(&err));
        else
            rsvg_xml_set_error(error, handle->priv->ctxt);

        goto out;
    }

    if (err != NULL) {
        g_propagate_error(error, g_steal_pointer(&err));
        goto out;
    }

    res = TRUE;

out:

    priv->ctxt = rsvg_free_xml_parser_and_doc(priv->ctxt);
    if (res && err != NULL) {
        g_propagate_error(error, g_steal_pointer(&err));
        res = FALSE;
    }

    priv->error = NULL;
    g_clear_object(&priv->cancellable);

    if (res) {
        priv->state = RSVG_HANDLE_STATE_CLOSED_OK;
    }
    else {
        priv->state = RSVG_HANDLE_STATE_CLOSED_ERROR;
    }

    return res;
}

/**
 * rsvg_handle_new_from_gfile_sync:
 * @file: a #GFile
 * @flags: flags from #RsvgHandleFlags
 * @cancellable: (allow-none): a #GCancellable, or %NULL
 * @error: (allow-none): a location to store a #GError, or %NULL
 *
 * Creates a new #RsvgHandle for @file.
 *
 * If @cancellable is not %NULL, then the operation can be cancelled by
 * triggering the cancellable object from another thread. If the
 * operation was cancelled, the error %G_IO_ERROR_CANCELLED will be
 * returned.
 *
 * Returns: a new #RsvgHandle on success, or %NULL with @error filled in
 *
 * Since: 2.32
 */
RsvgHandle* rsvg_handle_new_from_gfile_sync(GFile* file,
                                            RsvgHandleFlags flags,
                                            GCancellable* cancellable,
                                            GError** error) {
    RsvgHandle* handle;
    g_autoptr(GFileInputStream) stream = NULL;

    g_return_val_if_fail(G_IS_FILE(file), NULL);
    g_return_val_if_fail(cancellable == NULL || G_IS_CANCELLABLE(cancellable), NULL);
    g_return_val_if_fail(error == NULL || *error == NULL, NULL);

    stream = g_file_read(file, cancellable, error);
    if (stream == NULL)
        return NULL;

    handle = rsvg_handle_new_from_stream_sync(G_INPUT_STREAM(stream), file, flags, cancellable, error);

    return handle;
}

/**
 * rsvg_handle_new_from_stream_sync:
 * @input_stream: a #GInputStream
 * @base_file: (allow-none): a #GFile, or %NULL
 * @flags: flags from #RsvgHandleFlags
 * @cancellable: (allow-none): a #GCancellable, or %NULL
 * @error: (allow-none): a location to store a #GError, or %NULL
 *
 * Creates a new #RsvgHandle for @stream.
 *
 * If @cancellable is not %NULL, then the operation can be cancelled by
 * triggering the cancellable object from another thread. If the
 * operation was cancelled, the error %G_IO_ERROR_CANCELLED will be
 * returned.
 *
 * Returns: a new #RsvgHandle on success, or %NULL with @error filled in
 *
 * Since: 2.32
 */
RsvgHandle* rsvg_handle_new_from_stream_sync(GInputStream* input_stream,
                                             GFile* base_file,
                                             RsvgHandleFlags flags,
                                             GCancellable* cancellable,
                                             GError** error) {
    RsvgHandle* handle;

    g_return_val_if_fail(G_IS_INPUT_STREAM(input_stream), NULL);
    g_return_val_if_fail(base_file == NULL || G_IS_FILE(base_file), NULL);
    g_return_val_if_fail(cancellable == NULL || G_IS_CANCELLABLE(cancellable), NULL);
    g_return_val_if_fail(error == NULL || *error == NULL, NULL);

    handle = rsvg_handle_new_with_flags(flags);

    if (base_file)
        rsvg_handle_set_base_gfile(handle, base_file);

    if (!rsvg_handle_read_stream_sync(handle, input_stream, cancellable, error)) {
        g_object_unref(handle);
        return NULL;
    }

    return handle;
}

/**
 * _rsvg_handle_internal_set_testing:
 * @handle: a #RsvgHandle
 * @testing: Whether to enable testing mode
 *
 * Do not call this function.  This is intended for librsvg's internal
 * test suite only.
 **/
void rsvg_handle_internal_set_testing(RsvgHandle* handle, gboolean testing) {
    g_return_if_fail(RSVG_IS_HANDLE(handle));

    handle->priv->is_testing = testing ? TRUE : FALSE;
}

/**
 * rsvg_init:
 *
 * This function does nothing.
 * Since: 2.9
 * Deprecated: 2.36: Use g_type_init()
 **/
void rsvg_init(void) {}

/**
 * rsvg_term:
 *
 * This function does nothing.
 *
 * Since: 2.9
 * Deprecated: 2.36
 **/
void rsvg_term(void) {}

/**
 * rsvg_cleanup:
 *
 * This function should not be called from normal programs.
 * See xmlCleanupParser() for more information.
 *
 * Since: 2.36
 **/
void rsvg_cleanup(void) {
    xmlCleanupParser();
}

void rsvg_node_set_atts(RsvgNode* node, RsvgHandle* ctx, RsvgPropertyBag* atts) {
    node->set_atts(node, ctx, atts);
}

void rsvg_pop_discrete_layer(RsvgDrawingCtx* ctx) {
    ctx->render->pop_discrete_layer(ctx);
}

void rsvg_push_discrete_layer(RsvgDrawingCtx* ctx) {
    ctx->render->push_discrete_layer(ctx);
}

void rsvg_drawing_ctx_increase_num_elements_acquired(RsvgDrawingCtx* draw_ctx) {
    draw_ctx->num_elements_acquired++;
}

/* This is a mitigation for the security-related bugs:
 * https://gitlab.gnome.org/GNOME/librsvg/issues/323
 * https://gitlab.gnome.org/GNOME/librsvg/issues/515
 *
 * Imagine the XML [billion laughs attack], but done in SVG's terms:
 *
 * - #323 above creates deeply nested groups of `<use>` elements.
 * The first one references the second one ten times, the second one
 * references the third one ten times, and so on.  In the file given,
 * this causes 10^17 objects to be rendered.  While this does not
 * exhaust memory, it would take a really long time.
 *
 * - #515 has deeply nested references of `<pattern>` elements.  Each
 * object inside each pattern has an attribute
 * fill="url(#next_pattern)", so the number of final rendered objects
 * grows exponentially.
 *
 * We deal with both cases by placing a limit on how many references
 * will be resolved during the SVG rendering process, that is,
 * how many `url(#foo)` will be resolved.
 *
 * [billion laughs attack]: https://bitbucket.org/tiran/defusedxml
 */
gboolean rsvg_drawing_ctx_limits_exceeded(RsvgDrawingCtx* draw_ctx) {
    /* Hard limit for "billion laughs" mitigation.
     * 1024 is enough for reasonably complex files while stopping exponential attacks quickly.
     */
    return draw_ctx->num_elements_acquired > 1024;
}

RsvgNode* rsvg_drawing_ctx_acquire_node_ref(RsvgDrawingCtx* ctx, RsvgNode* node) {
    if (g_slist_find(ctx->acquired_nodes, node))
        return NULL;

    ctx->acquired_nodes = g_slist_prepend(ctx->acquired_nodes, node);

    return node;
}

/*
 * rsvg_acquire_node:
 * @ctx: The drawing context in use
 * @url: The IRI to lookup
 *
 * Use this function when looking up urls to other nodes. This
 * function does proper recursion checking and thereby avoids
 * infinite loops.
 *
 * Nodes acquired by this function must be released using
 * rsvg_release_node() in reverse acquiring order.
 *
 * Returns: The node referenced by @url or %NULL if the @url
 *          does not reference a node.
 */
RsvgNode* rsvg_acquire_node(RsvgDrawingCtx* ctx, const char* url) {
    RsvgNode* node;

    if (url == NULL)
        return NULL;

    rsvg_drawing_ctx_increase_num_elements_acquired(ctx);
    if (rsvg_drawing_ctx_limits_exceeded(ctx))
        return NULL;

    node = rsvg_defs_lookup(ctx->defs, url);
    if (node == NULL)
        return NULL;

    return rsvg_drawing_ctx_acquire_node_ref(ctx, node);
}

/*
 * rsvg_release_node:
 * @ctx: The drawing context the node was acquired from
 * @node: Node to release
 *
 * Releases a node previously acquired via rsvg_acquire_node().
 *
 * if @node is %NULL, this function does nothing.
 */
void rsvg_release_node(RsvgDrawingCtx* ctx, RsvgNode* node) {
    if (node == NULL)
        return;

    g_return_if_fail(ctx->acquired_nodes != NULL);
    g_return_if_fail(ctx->acquired_nodes->data == node);

    ctx->acquired_nodes = g_slist_remove(ctx->acquired_nodes, node);
}

void rsvg_render_path(RsvgDrawingCtx* ctx, const cairo_path_t* path) {
    ctx->render->render_path(ctx, path);
    rsvg_render_markers(ctx, path);
}

void rsvg_render_surface(RsvgDrawingCtx* ctx,
                         cairo_surface_t* surface,
                         double x,
                         double y,
                         double w,
                         double h,
                         const cairo_rectangle_t* viewport) {
    /* surface must be a cairo image surface */
    g_return_if_fail(cairo_surface_get_type(surface) == CAIRO_SURFACE_TYPE_IMAGE);

    ctx->render->render_surface(ctx, surface, x, y, w, h);

    /* Record pre-fit geometry without changing the renderer callback's type. */
    if (ctx->render->type == RSVG_RENDER_TYPE_CAIRO) {
        RsvgCairoRender* render = RSVG_CAIRO_RENDER(ctx->render);
        if (render->measure_logical && cairo_image_surface_get_width(surface) > 0 &&
            cairo_image_surface_get_height(surface) > 0) {
            RsvgBbox logical;
            rsvg_bbox_init(&logical, &rsvg_current_state(ctx)->affine);
            logical.rect = *viewport;
            logical.virgin = 0;
            rsvg_bbox_insert(&render->bounds.logical, &logical);
        }
    }
}

void rsvg_add_clipping_rect(RsvgDrawingCtx* ctx, double x, double y, double w, double h) {
    ctx->render->add_clipping_rect(ctx, x, y, w, h);
}

cairo_surface_t* rsvg_get_surface_of_node(RsvgDrawingCtx* ctx, RsvgNode* drawable, double w, double h) {
    return ctx->render->get_surface_of_node(ctx, drawable, w, h);
}

void rsvg_render_free(RsvgRender* render) {
    render->free(render);
}

void rsvg_bbox_init(RsvgBbox* self, cairo_matrix_t* affine) {
    self->virgin = 1;
    self->affine = *affine;
}

static gboolean rsvg_bbox_has_same_affine(const RsvgBbox* dst, const RsvgBbox* src) {
    return dst->affine.xx == src->affine.xx && dst->affine.yx == src->affine.yx && dst->affine.xy == src->affine.xy &&
           dst->affine.yy == src->affine.yy && dst->affine.x0 == src->affine.x0 && dst->affine.y0 == src->affine.y0;
}

void rsvg_bbox_insert(RsvgBbox* dst, RsvgBbox* src) {
    cairo_matrix_t affine;
    double xmin, ymin;
    double xmax, ymax;
    int i;

    if (src->virgin)
        return;

    if (!dst->virgin) {
        xmin = dst->rect.x, ymin = dst->rect.y;
        xmax = dst->rect.x + dst->rect.width, ymax = dst->rect.y + dst->rect.height;
    }
    else {
        xmin = ymin = xmax = ymax = 0;
    }

    affine = dst->affine;
    if (cairo_matrix_invert(&affine) != CAIRO_STATUS_SUCCESS) {
        if (!rsvg_bbox_has_same_affine(dst, src))
            return;

        if (dst->virgin) {
            dst->rect = src->rect;
            dst->virgin = 0;
        }
        else {
            xmin = dst->rect.x;
            ymin = dst->rect.y;
            xmax = dst->rect.x + dst->rect.width;
            ymax = dst->rect.y + dst->rect.height;

            if (src->rect.x < xmin)
                xmin = src->rect.x;
            if (src->rect.y < ymin)
                ymin = src->rect.y;
            if (src->rect.x + src->rect.width > xmax)
                xmax = src->rect.x + src->rect.width;
            if (src->rect.y + src->rect.height > ymax)
                ymax = src->rect.y + src->rect.height;

            dst->rect.x = xmin;
            dst->rect.y = ymin;
            dst->rect.width = xmax - xmin;
            dst->rect.height = ymax - ymin;
        }

        return;
    }

    cairo_matrix_multiply(&affine, &src->affine, &affine);

    for (i = 0; i < 4; i++) {
        double rx, ry, x, y;
        rx = src->rect.x + src->rect.width * (double)(i % 2);
        ry = src->rect.y + src->rect.height * (double)(i / 2);
        x = affine.xx * rx + affine.xy * ry + affine.x0;
        y = affine.yx * rx + affine.yy * ry + affine.y0;
        if (dst->virgin) {
            xmin = xmax = x;
            ymin = ymax = y;
            dst->virgin = 0;
        }
        else {
            if (x < xmin)
                xmin = x;
            if (x > xmax)
                xmax = x;
            if (y < ymin)
                ymin = y;
            if (y > ymax)
                ymax = y;
        }
    }
    dst->rect.x = xmin;
    dst->rect.y = ymin;
    dst->rect.width = xmax - xmin;
    dst->rect.height = ymax - ymin;
}

void rsvg_bbox_clip(RsvgBbox* dst, RsvgBbox* src) {
    cairo_matrix_t affine;
    double xmin, ymin;
    double xmax, ymax;
    int i;

    if (src->virgin)
        return;

    if (!dst->virgin) {
        xmin = dst->rect.x + dst->rect.width, ymin = dst->rect.y + dst->rect.height;
        xmax = dst->rect.x, ymax = dst->rect.y;
    }
    else {
        xmin = ymin = xmax = ymax = 0;
    }

    affine = dst->affine;
    if (cairo_matrix_invert(&affine) != CAIRO_STATUS_SUCCESS)
        return;

    cairo_matrix_multiply(&affine, &src->affine, &affine);

    for (i = 0; i < 4; i++) {
        double rx, ry, x, y;
        rx = src->rect.x + src->rect.width * (double)(i % 2);
        ry = src->rect.y + src->rect.height * (double)(i / 2);
        x = affine.xx * rx + affine.xy * ry + affine.x0;
        y = affine.yx * rx + affine.yy * ry + affine.y0;
        if (dst->virgin) {
            xmin = xmax = x;
            ymin = ymax = y;
            dst->virgin = 0;
        }
        else {
            if (x < xmin)
                xmin = x;
            if (x > xmax)
                xmax = x;
            if (y < ymin)
                ymin = y;
            if (y > ymax)
                ymax = y;
        }
    }

    if (xmin < dst->rect.x)
        xmin = dst->rect.x;
    if (ymin < dst->rect.y)
        ymin = dst->rect.y;
    if (xmax > dst->rect.x + dst->rect.width)
        xmax = dst->rect.x + dst->rect.width;
    if (ymax > dst->rect.y + dst->rect.height)
        ymax = dst->rect.y + dst->rect.height;

    dst->rect.x = xmin;
    dst->rect.width = xmax - xmin;
    dst->rect.y = ymin;
    dst->rect.height = ymax - ymin;
}

void _rsvg_push_view_box(RsvgDrawingCtx* ctx, double w, double h) {
    RsvgViewBox* vb = g_new(RsvgViewBox, 1);
    *vb = ctx->vb;
    ctx->vb_stack = g_slist_prepend(ctx->vb_stack, vb);
    ctx->vb.rect.width = w;
    ctx->vb.rect.height = h;
}

void _rsvg_pop_view_box(RsvgDrawingCtx* ctx) {
    ctx->vb = *((RsvgViewBox*)ctx->vb_stack->data);
    g_free(ctx->vb_stack->data);
    ctx->vb_stack = g_slist_delete_link(ctx->vb_stack, ctx->vb_stack);
}

void rsvg_return_if_fail_warning(const char* pretty_function, const char* expression, GError** error) {
    g_set_error(error, RSVG_ERROR, 0, _("%s: assertion `%s' failed"), pretty_function, expression);
}

#ifdef HAVE_RSVG_ROOTED_FILE_IO
/* Consume the directory descriptor, retaining at most two descriptors while
 * walking canonical components. Replacement symlinks must not redirect a read.
 */
static int rsvg_open_components(int fd, char* path, gboolean directory, GCancellable* cancellable, GError** error) {
    char* component = path;
    while (*component) {
        char* slash = strchr(component, '/');
        int flags = O_NOFOLLOW | O_CLOEXEC;
        int next, saved_errno;
        if (g_cancellable_set_error_if_cancelled(cancellable, error)) {
            close(fd);
            return -1;
        }
        if (slash)
            *slash = '\0';
        flags |= slash || directory ? O_PATH | O_DIRECTORY : O_RDONLY;
        next = openat(fd, component, flags);
        saved_errno = errno;
        if (slash)
            *slash = '/';
        close(fd);
        if (next < 0) {
            g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved_errno), "Cannot open resource component: %s",
                        g_strerror(saved_errno));
            return -1;
        }
        fd = next;
        if (!slash)
            break;
        component = slash + 1;
    }
    return fd;
}

static char* rsvg_read_file_descriptor(int fd,
                                       const char* uri,
                                       char** mime_type,
                                       gsize* out_len,
                                       GCancellable* cancellable,
                                       GError** error) {
    g_autoptr(GInputStream) input = g_unix_input_stream_new(fd, TRUE);
    g_autoptr(GOutputStream) output = g_memory_output_stream_new(NULL, 0, g_try_realloc, g_free);
    gsize len;
    char* data;

    if (g_output_stream_splice(output, input, G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE, cancellable, error) < 0)
        return NULL;
    len = g_memory_output_stream_get_data_size(G_MEMORY_OUTPUT_STREAM(output));
    if (!g_output_stream_write_all(output, "", 1, NULL, cancellable, error) ||
        !g_output_stream_close(output, cancellable, error))
        return NULL;
    data = g_memory_output_stream_steal_data(G_MEMORY_OUTPUT_STREAM(output));
    if (mime_type) {
        g_autoptr(GFile) file = g_file_new_for_uri(uri);
        g_autofree char* path = g_file_get_path(file);
        g_autofree char* type = g_content_type_guess(path, (guchar*)data, len, NULL);
        *mime_type = type ? g_content_type_get_mime_type(type) : NULL;
    }
    if (out_len)
        *out_len = len;
    return data;
}
#endif

static gboolean rsvg_allow_load(GFile* base_gfile,
                                const char* uri,
                                int* file_fd,
                                GCancellable* cancellable,
                                GError** error) {
    GFile* base;
    char *path, *dir;
    char *scheme = NULL, *cpath = NULL, *cdir = NULL;

    g_assert(error == NULL || *error == NULL);
    *file_fd = -1;
#ifndef HAVE_RSVG_ROOTED_FILE_IO
    (void)cancellable;
#endif

    scheme = g_uri_parse_scheme(uri);

    /* Not a valid URI */
    if (scheme == NULL)
        goto deny;

    /* Allow loads of data: from any location */
    if (g_str_equal(scheme, "data"))
        goto allow;

    /* No base to compare to? */
    if (base_gfile == NULL)
        goto deny;

    /* Deny loads from differing URI schemes */
    if (!g_file_has_uri_scheme(base_gfile, scheme))
        goto deny;

    /* resource: is allowed to load anything from other resources */
    if (g_str_equal(scheme, "resource"))
        goto allow;

    /* Non-file: isn't allowed to load anything */
    if (!g_str_equal(scheme, "file"))
        goto deny;

    base = g_file_get_parent(base_gfile);
    if (base == NULL)
        goto deny;

    dir = g_file_get_path(base);
    g_object_unref(base);
    if (dir == NULL)
        goto deny;

    cdir = realpath(dir, NULL);
    g_free(dir);
    if (cdir == NULL)
        goto deny;

#ifdef HAVE_RSVG_ROOTED_FILE_IO
    GFile* target = g_file_new_for_uri(uri);
    path = g_file_get_path(target);
    g_object_unref(target);
#else
    path = g_filename_from_uri(uri, NULL, NULL);
#endif
    if (path == NULL)
        goto deny;

    cpath = realpath(path, NULL);
    g_free(path);

    if (cpath == NULL)
        goto deny;

    /* Now check that @cpath is below @cdir */
    if (!g_str_has_prefix(cpath, cdir) || cpath[strlen(cdir)] != G_DIR_SEPARATOR)
        goto deny;

#ifdef HAVE_RSVG_ROOTED_FILE_IO
    /* Walk the canonical base from the filesystem root as well: changing a
     * base ancestor to a symlink must not redirect the directory capability.
     */
    int fd = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        int saved_errno = errno;
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved_errno), "Cannot open filesystem root: %s",
                    g_strerror(saved_errno));
        goto failed;
    }
    fd = rsvg_open_components(fd, cdir + 1, TRUE, cancellable, error);
    if (fd < 0)
        goto failed;
    fd = rsvg_open_components(fd, cpath + strlen(cdir) + 1, FALSE, cancellable, error);
    if (fd < 0)
        goto failed;
    *file_fd = fd;
#endif

allow:
    g_free(scheme);
    free(cpath);
    free(cdir);
    return TRUE;

deny:
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "File may not link to URI \"%s\"", uri);
#ifdef HAVE_RSVG_ROOTED_FILE_IO
failed:
#endif
    g_free(scheme);
    free(cpath);
    free(cdir);

    return FALSE;
}

char* rsvg_handle_resolve_uri(RsvgHandle* handle, const char* uri) {
    RsvgHandlePrivate* priv = handle->priv;
    char *scheme, *resolved_uri;
    GFile *base, *resolved;

    if (uri == NULL)
        return NULL;

    scheme = g_uri_parse_scheme(uri);
    if (scheme != NULL || priv->base_gfile == NULL || (base = g_file_get_parent(priv->base_gfile)) == NULL) {
        g_free(scheme);
        return g_strdup(uri);
    }

    resolved = g_file_resolve_relative_path(base, uri);
    resolved_uri = g_file_get_uri(resolved);

    g_free(scheme);
    g_object_unref(base);
    g_object_unref(resolved);

    return resolved_uri;
}

/* Both resource readers must authorize the resolved URI before any I/O.
 * Top-level file/stream APIs are caller-authorized and do not use this gate.
 */
static char* rsvg_handle_resolve_resource_uri(RsvgHandle* handle, const char* url, int* file_fd, GError** error) {
    char* uri;

    if (url == NULL || *url == '\0') {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid resource URI");
        return NULL;
    }

    uri = rsvg_handle_resolve_uri(handle, url);
    if (!rsvg_allow_load(handle->priv->base_gfile, uri, file_fd, handle->priv->cancellable, error)) {
        g_free(uri);
        return NULL;
    }

    return uri;
}

char* _rsvg_handle_acquire_data(RsvgHandle* handle, const char* url, char** content_type, gsize* len, GError** error) {
    int fd = -1;
    g_autofree char* uri = rsvg_handle_resolve_resource_uri(handle, url, &fd, error);

    if (uri == NULL)
        return NULL;

#ifdef HAVE_RSVG_ROOTED_FILE_IO
    if (fd >= 0)
        return rsvg_read_file_descriptor(fd, uri, content_type, len, handle->priv->cancellable, error);
#endif
    return _rsvg_io_acquire_data(uri, rsvg_handle_get_base_uri(handle), content_type, len, handle->priv->cancellable,
                                 error);
}

GInputStream* _rsvg_handle_acquire_stream(RsvgHandle* handle, const char* url, char** content_type, GError** error) {
    int fd = -1;
    g_autofree char* uri = rsvg_handle_resolve_resource_uri(handle, url, &fd, error);

    if (uri == NULL)
        return NULL;

#ifdef HAVE_RSVG_ROOTED_FILE_IO
    if (fd >= 0) {
        gsize size;
        char* data = rsvg_read_file_descriptor(fd, uri, content_type, &size, handle->priv->cancellable, error);
        return data ? g_memory_input_stream_new_from_data(data, size, g_free) : NULL;
    }
#endif
    return _rsvg_io_acquire_stream(uri, rsvg_handle_get_base_uri(handle), content_type, handle->priv->cancellable,
                                   error);
}

/* Frees the ctxt and its ctxt->myDoc - libxml2 doesn't free them together
 * http://xmlsoft.org/html/libxml-parser.html#xmlFreeParserCtxt
 *
 * Returns NULL.
 */
xmlParserCtxtPtr rsvg_free_xml_parser_and_doc(xmlParserCtxtPtr ctxt) {
    if (ctxt) {
        if (ctxt->myDoc) {
            xmlFreeDoc(ctxt->myDoc);
            ctxt->myDoc = NULL;
        }

        xmlFreeParserCtxt(ctxt);
    }

    return NULL;
}
