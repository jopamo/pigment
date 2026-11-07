/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* vim: set sw=4 sts=4 ts=4 expandtab: */
/*
   rsvg-text.c: Text handling routines for RSVG

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

#include <math.h>
#include <string.h>

#include "rsvg-private.h"
#include "rsvg-styles.h"
#include "rsvg-text.h"
#include "rsvg-css.h"

#include "rsvg-shapes.h"

/* what we use for text rendering depends on what cairo has to offer */
#include <pango/pangocairo.h>
#include <hb-ot.h>

typedef struct _RsvgNodeText RsvgNodeText;

struct _RsvgNodeText {
    RsvgNode super;
    RsvgLength x, y, dx, dy;
};

typedef struct {
    PangoGravity gravity;
    PangoDirection writing_direction;
    PangoDirection direction;
} RsvgTextChunk;

gboolean rsvg_text_get_font_shift_factor(hb_font_t* font, gboolean superscript, double* factor) {
    if (!font)
        return FALSE;
    hb_face_t* face = hb_font_get_face(font);
    unsigned upem = hb_face_get_upem(face);
    if (!upem)
        return FALSE;
    hb_blob_t* table = hb_face_reference_table(face, HB_TAG('O', 'S', '/', '2'));
    unsigned length;
    const unsigned char* data = (const unsigned char*)hb_blob_get_data(table, &length);
    if (length < 26) {
        hb_blob_destroy(table);
        return FALSE;
    }
    unsigned index = superscript ? 24 : 16;
    int offset = (data[index] << 8) | data[index + 1];
    if (offset & 0x8000)
        offset -= 0x10000;
    hb_blob_destroy(table);
    hb_ot_metrics_tag_t tag =
        superscript ? HB_OT_METRICS_TAG_SUPERSCRIPT_EM_Y_OFFSET : HB_OT_METRICS_TAG_SUBSCRIPT_EM_Y_OFFSET;
    /* Keep fractional deltas in font units, before Pango-scale rounding. */
    double adjusted = offset + (double)hb_ot_metrics_get_variation(font, tag);
    *factor = (superscript ? adjusted : -adjusted) / upem;
    return TRUE;
}

char* rsvg_make_valid_utf8(const char* str, int len) {
    GString* string;
    const char *remainder, *invalid;
    int remaining_bytes, valid_bytes;

    string = NULL;
    remainder = str;

    if (len < 0)
        remaining_bytes = strlen(str);
    else
        remaining_bytes = len;

    while (remaining_bytes != 0) {
        if (g_utf8_validate(remainder, remaining_bytes, &invalid))
            break;
        valid_bytes = invalid - remainder;

        if (string == NULL)
            string = g_string_sized_new(remaining_bytes);

        g_string_append_len(string, remainder, valid_bytes);
        g_string_append_c(string, '?');

        remaining_bytes -= valid_bytes + 1;
        remainder = invalid + 1;
    }

    if (string == NULL)
        return len < 0 ? g_strdup(str) : g_strndup(str, len);

    g_string_append_len(string, remainder, remaining_bytes);

    return g_string_free(string, FALSE);
}

static GString* _rsvg_text_chomp(RsvgState* state, GString* in, gboolean first, gboolean last) {
    GString* out;
    guint i;
    gboolean lastwasspace = first;
    out = g_string_new(in->str);

    if (!state->space_preserve) {
        for (i = 0; i < out->len;) {
            if (out->str[i] == '\n')
                g_string_erase(out, i, 1);
            else
                i++;
        }

        for (i = 0; i < out->len; i++)
            if (out->str[i] == '\t')
                out->str[i] = ' ';

        for (i = 0; i < out->len;) {
            if (out->str[i] == ' ' && lastwasspace)
                g_string_erase(out, i, 1);
            else {
                if (out->str[i] == ' ')
                    lastwasspace = TRUE;
                else
                    lastwasspace = FALSE;
                i++;
            }
        }
        if (last && out->len && out->str[out->len - 1] == ' ')
            g_string_truncate(out, out->len - 1);
    }
    else {
        for (i = 0; i < out->len; i++)
            if (out->str[i] == '\n' || out->str[i] == '\t')
                out->str[i] = ' ';
    }

    return out;
}

static void _rsvg_node_text_set_atts(RsvgNode* self, RsvgHandle* ctx, RsvgPropertyBag* atts) {
    const char *klazz = NULL, *id = NULL, *value;
    RsvgNodeText* text = (RsvgNodeText*)self;

    if (rsvg_property_bag_size(atts)) {
        if ((value = rsvg_property_bag_lookup(atts, "x")))
            text->x = _rsvg_css_parse_length(value);
        if ((value = rsvg_property_bag_lookup(atts, "y")))
            text->y = _rsvg_css_parse_length(value);
        if ((value = rsvg_property_bag_lookup(atts, "dx")))
            text->dx = _rsvg_css_parse_length(value);
        if ((value = rsvg_property_bag_lookup(atts, "dy")))
            text->dy = _rsvg_css_parse_length(value);
        if ((value = rsvg_property_bag_lookup(atts, "class")))
            klazz = value;
        if ((value = rsvg_property_bag_lookup(atts, "id"))) {
            id = value;
            rsvg_defs_register_name(ctx->priv->defs, value, self);
        }

        rsvg_parse_style_attrs(ctx, self->state, "text", klazz, id, atts);
    }
}

static void rsvg_text_render_text(RsvgDrawingCtx* ctx,
                                  const char* text,
                                  gdouble* x,
                                  gdouble* y,
                                  const RsvgTextChunk* chunk);

static void _rsvg_node_text_type_tspan(RsvgNodeText* self,
                                       RsvgDrawingCtx* ctx,
                                       gdouble* x,
                                       gdouble* y,
                                       gboolean usetextonly,
                                       RsvgTextChunk* chunk);

/* This function is responsible of selecting render for a text element including its children and giving it the drawing
 * context */
static void _rsvg_node_text_type_children(RsvgNode* self,
                                          RsvgDrawingCtx* ctx,
                                          gdouble* x,
                                          gdouble* y,
                                          gboolean usetextonly,
                                          RsvgTextChunk* chunk) {
    guint i;

    rsvg_push_discrete_layer(ctx);
    for (i = 0; i < self->children->len; i++) {
        RsvgNode* node = g_ptr_array_index(self->children, i);
        RsvgNodeType type = RSVG_NODE_TYPE(node);

        if (type == RSVG_NODE_TYPE_CHARS) {
            RsvgNodeChars* chars = (RsvgNodeChars*)node;
            GString* str =
                _rsvg_text_chomp(rsvg_current_state(ctx), chars->contents, i == 0, i + 1 == self->children->len);
            rsvg_text_render_text(ctx, str->str, x, y, chunk);
            g_string_free(str, TRUE);
        }
        else {
            if (usetextonly) {
                _rsvg_node_text_type_children(node, ctx, x, y, usetextonly, chunk);
            }
            else {
                if (type == RSVG_NODE_TYPE_TSPAN) {
                    RsvgNodeText* tspan = (RsvgNodeText*)node;
                    rsvg_state_push(ctx);
                    _rsvg_node_text_type_tspan(tspan, ctx, x, y, usetextonly, chunk);
                    rsvg_state_pop(ctx);
                }
            }
        }
    }
    rsvg_pop_discrete_layer(ctx);
}

static int _rsvg_node_text_length_tspan(RsvgNodeText* self,
                                        RsvgDrawingCtx* ctx,
                                        gdouble* x,
                                        gboolean usetextonly,
                                        const RsvgTextChunk* chunk);

static gdouble rsvg_text_length_text_as_string(RsvgDrawingCtx* ctx, const char* text, const RsvgTextChunk* chunk);

static int _rsvg_node_text_length_children(RsvgNode* self,
                                           guint start,
                                           RsvgDrawingCtx* ctx,
                                           gdouble* length,
                                           gboolean usetextonly,
                                           const RsvgTextChunk* chunk) {
    guint i;
    int out = FALSE;
    for (i = start; i < self->children->len; i++) {
        RsvgNode* node = g_ptr_array_index(self->children, i);
        RsvgNodeType type = RSVG_NODE_TYPE(node);

        rsvg_state_push(ctx);
        rsvg_state_reinherit_top(ctx, node->state, 0);
        if (type == RSVG_NODE_TYPE_CHARS) {
            RsvgNodeChars* chars = (RsvgNodeChars*)node;
            GString* str =
                _rsvg_text_chomp(rsvg_current_state(ctx), chars->contents, i == 0, i + 1 == self->children->len);
            *length += rsvg_text_length_text_as_string(ctx, str->str, chunk);
            g_string_free(str, TRUE);
        }
        else {
            if (usetextonly) {
                out = _rsvg_node_text_length_children(node, 0, ctx, length, usetextonly, chunk);
            }
            else {
                if (type == RSVG_NODE_TYPE_TSPAN) {
                    RsvgNodeText* tspan = (RsvgNodeText*)node;
                    out = _rsvg_node_text_length_tspan(tspan, ctx, length, usetextonly, chunk);
                }
            }
        }
        rsvg_state_pop(ctx);
        if (out)
            break;
    }
    return out;
}

static gdouble rsvg_text_chunk_length(RsvgNode* node,
                                      RsvgDrawingCtx* ctx,
                                      gboolean usetextonly,
                                      const RsvgTextChunk* chunk) {
    gdouble length = 0;
    guint start = 0;
    RsvgState* saved = rsvg_current_state(ctx);
    RsvgState* ancestor = saved;

    while (node && ancestor) {
        /* Measurement pushes its own frames; borrow the ancestor for following siblings. */
        ctx->state = ancestor;
        if (_rsvg_node_text_length_children(node, start, ctx, &length, usetextonly, chunk) ||
            RSVG_NODE_TYPE(node) == RSVG_NODE_TYPE_TEXT)
            break;

        RsvgNode* parent = node->parent;
        if (!parent)
            break;
        for (start = 0; start < parent->children->len; start++)
            if (g_ptr_array_index(parent->children, start) == node)
                break;
        g_assert(start < parent->children->len);
        start++;
        node = parent;
        ancestor = ancestor->parent;
    }
    ctx->state = saved;
    return length;
}

static void _rsvg_node_text_draw(RsvgNode* self, RsvgDrawingCtx* ctx, int dominate) {
    double x, y, dx, dy, length = 0;
    RsvgNodeText* text = (RsvgNodeText*)self;
    rsvg_state_reinherit_top(ctx, self->state, dominate);
    RsvgState* state = rsvg_current_state(ctx);
    StyleValueData* mode = g_hash_table_lookup(state->styles, "writing-mode");
    RsvgTextChunk chunk = {state->text_gravity,
                           mode && (g_str_equal(mode->value, "rl") || g_str_equal(mode->value, "rl-tb"))
                               ? PANGO_DIRECTION_RTL
                               : PANGO_DIRECTION_LTR,
                           state->text_dir};

    x = _rsvg_css_normalize_length(&text->x, ctx, 'h');
    y = _rsvg_css_normalize_length(&text->y, ctx, 'v');
    dx = _rsvg_css_normalize_length(&text->dx, ctx, 'h');
    dy = _rsvg_css_normalize_length(&text->dy, ctx, 'v');

    if (rsvg_current_state(ctx)->text_anchor != TEXT_ANCHOR_START) {
        _rsvg_node_text_length_children(self, 0, ctx, &length, FALSE, &chunk);
        if (rsvg_current_state(ctx)->text_anchor == TEXT_ANCHOR_MIDDLE)
            length /= 2;
    }
    if (PANGO_GRAVITY_IS_VERTICAL(chunk.gravity)) {
        y -= length;
    }
    else {
        if (chunk.direction == PANGO_DIRECTION_RTL)
            length = -length;
        x -= length;
    }
    x += dx;
    y += dy;

    _rsvg_node_text_type_children(self, ctx, &x, &y, FALSE, &chunk);
}

RsvgNode* rsvg_new_text(void) {
    RsvgNodeText* text;
    text = g_new(RsvgNodeText, 1);
    _rsvg_node_init(&text->super, RSVG_NODE_TYPE_TEXT);
    text->super.draw = _rsvg_node_text_draw;
    text->super.set_atts = _rsvg_node_text_set_atts;
    text->x = text->y = text->dx = text->dy = _rsvg_css_parse_length("0");
    return &text->super;
}

static void _rsvg_node_text_type_tspan(RsvgNodeText* self,
                                       RsvgDrawingCtx* ctx,
                                       gdouble* x,
                                       gdouble* y,
                                       gboolean usetextonly,
                                       RsvgTextChunk* chunk) {
    double dx, dy, length = 0;
    gboolean positioned = self->x.factor != 'n' || self->y.factor != 'n';
    rsvg_state_reinherit_top(ctx, self->super.state, 0);
    if (positioned)
        chunk->direction = rsvg_current_state(ctx)->text_dir;

    dx = _rsvg_css_normalize_length(&self->dx, ctx, 'h');
    dy = _rsvg_css_normalize_length(&self->dy, ctx, 'v');

    if (positioned && rsvg_current_state(ctx)->text_anchor != TEXT_ANCHOR_START) {
        length = rsvg_text_chunk_length(&self->super, ctx, usetextonly, chunk);
        if (rsvg_current_state(ctx)->text_anchor == TEXT_ANCHOR_MIDDLE)
            length /= 2;
    }

    if (self->x.factor != 'n')
        *x = _rsvg_css_normalize_length(&self->x, ctx, 'h');
    if (self->y.factor != 'n')
        *y = _rsvg_css_normalize_length(&self->y, ctx, 'v');
    if (positioned) {
        if (PANGO_GRAVITY_IS_VERTICAL(chunk->gravity))
            *y -= length;
        else {
            if (chunk->direction == PANGO_DIRECTION_RTL)
                length = -length;
            *x -= length;
        }
    }
    *x += dx;
    *y += dy;
    _rsvg_node_text_type_children(&self->super, ctx, x, y, usetextonly, chunk);
}

static int _rsvg_node_text_length_tspan(RsvgNodeText* self,
                                        RsvgDrawingCtx* ctx,
                                        gdouble* length,
                                        gboolean usetextonly,
                                        const RsvgTextChunk* chunk) {
    if (self->x.factor != 'n' || self->y.factor != 'n')
        return TRUE;

    if (PANGO_GRAVITY_IS_VERTICAL(chunk->gravity))
        *length += _rsvg_css_normalize_length(&self->dy, ctx, 'v');
    else
        *length += (chunk->direction == PANGO_DIRECTION_RTL ? -1 : 1) * _rsvg_css_normalize_length(&self->dx, ctx, 'h');

    return _rsvg_node_text_length_children(&self->super, 0, ctx, length, usetextonly, chunk);
}

static void _rsvg_node_tspan_set_atts(RsvgNode* self, RsvgHandle* ctx, RsvgPropertyBag* atts) {
    const char *klazz = NULL, *id = NULL, *value;
    RsvgNodeText* text = (RsvgNodeText*)self;

    if (rsvg_property_bag_size(atts)) {
        if ((value = rsvg_property_bag_lookup(atts, "x")))
            text->x = _rsvg_css_parse_length(value);
        if ((value = rsvg_property_bag_lookup(atts, "y")))
            text->y = _rsvg_css_parse_length(value);
        if ((value = rsvg_property_bag_lookup(atts, "dx")))
            text->dx = _rsvg_css_parse_length(value);
        if ((value = rsvg_property_bag_lookup(atts, "dy")))
            text->dy = _rsvg_css_parse_length(value);
        if ((value = rsvg_property_bag_lookup(atts, "class")))
            klazz = value;
        if ((value = rsvg_property_bag_lookup(atts, "id"))) {
            id = value;
            rsvg_defs_register_name(ctx->priv->defs, value, self);
        }

        rsvg_parse_style_attrs(ctx, self->state, "tspan", klazz, id, atts);
    }
}

RsvgNode* rsvg_new_tspan(void) {
    RsvgNodeText* text;
    text = g_new(RsvgNodeText, 1);
    _rsvg_node_init(&text->super, RSVG_NODE_TYPE_TSPAN);
    text->super.set_atts = _rsvg_node_tspan_set_atts;
    text->x.factor = text->y.factor = 'n';
    text->dx = text->dy = _rsvg_css_parse_length("0");
    return &text->super;
}

typedef struct _RsvgTextLayout RsvgTextLayout;

struct _RsvgTextLayout {
    PangoLayout* layout;
    RsvgDrawingCtx* ctx;
    TextAnchor anchor;
    gdouble x, y;
};

static void rsvg_text_layout_free(RsvgTextLayout* layout) {
    g_object_unref(layout->layout);
    g_free(layout);
}

static double text_font_size(RsvgDrawingCtx* ctx, RsvgState* state) {
    double size = _rsvg_css_normalize_font_size(state, ctx);

    if (!isfinite(size) || size < 0.0 || size > (double)G_MAXINT / (2 * PANGO_SCALE))
        return 0.0;

    return size;
}

static gint text_pango_units(double value) {
    double units = value * PANGO_SCALE;

    if (!isfinite(units) || units < G_MININT || units > G_MAXINT)
        return 0;

    return (gint)units;
}

static PangoFontDescription* rsvg_text_font_description(RsvgDrawingCtx* ctx, RsvgState* state, PangoContext* context) {
    PangoFontDescription* font_desc = pango_font_description_copy(pango_context_get_font_description(context));
    if (state->font_family)
        pango_font_description_set_family_static(font_desc, state->font_family);
    pango_font_description_set_style(font_desc, state->font_style);
    pango_font_description_set_variant(font_desc, state->font_variant);
    pango_font_description_set_weight(font_desc, state->font_weight);
    pango_font_description_set_stretch(font_desc, state->font_stretch);
    pango_font_description_set_absolute_size(font_desc, text_font_size(ctx, state) * PANGO_SCALE);
    return font_desc;
}

static double rsvg_text_keyword_shift_factor(RsvgDrawingCtx* ctx,
                                             RsvgState* parent,
                                             PangoContext* context,
                                             double fallback) {
    if (fallback == 0.)
        return 0.;
    /* Select the parent's nominal font without changing the child layout's context. */
    PangoContext* parent_context = ctx->render->create_pango_context(ctx);
    pango_context_set_language(parent_context, parent->lang ? pango_language_from_string(parent->lang) : NULL);
    pango_context_set_base_gravity(parent_context, parent->text_gravity);
    PangoFontDescription* font_desc = rsvg_text_font_description(ctx, parent, context);
    PangoFont* font = pango_context_load_font(parent_context, font_desc);
    pango_font_description_free(font_desc);
    g_object_unref(parent_context);
    double factor = fallback;
    if (font) {
        hb_font_t* hb_font = pango_font_get_hb_font(font);
        if (hb_font)
            rsvg_text_get_font_shift_factor(hb_font, fallback > 0., &factor);
        g_object_unref(font);
    }
    return factor;
}

/* Add specified shifts without inheriting them a second time. */
static double rsvg_text_accumulate_baseline_shift(RsvgState* state, RsvgDrawingCtx* ctx, PangoContext* context) {
    RsvgState* parent = rsvg_state_parent(state);
    double shift = 0.;
    if (parent) {
        if (state->has_baseline_shift) {
            RsvgLength length;
            StyleValueData* data = g_hash_table_lookup(state->styles, "baseline-shift");
            if (data && rsvg_css_parse_baseline_shift(data->value, &length) && length.factor != 'b') {
                if (length.factor == 'i')
                    shift = length.length * rsvg_viewport_percentage(ctx->dpi_x, ctx->dpi_y);
                else if (length.factor == '\0')
                    shift = length.length;
                else {
                    double font_size = _rsvg_css_normalize_font_size(state, ctx);
                    shift = _rsvg_css_hand_normalize_length(&length, 0, font_size, font_size);
                }
            }
            else
                shift = _rsvg_css_normalize_font_size(parent, ctx) *
                        rsvg_text_keyword_shift_factor(ctx, parent, context, state->baseline_shift);
        }
        shift += rsvg_text_accumulate_baseline_shift(parent, ctx, context);
    }
    return shift;
}

static PangoLayout* rsvg_text_create_layout(RsvgDrawingCtx* ctx,
                                            RsvgState* state,
                                            const char* text,
                                            PangoContext* context,
                                            const RsvgTextChunk* chunk) {
    PangoDirection direction = state->text_dir;
    if (direction == PANGO_DIRECTION_LTR && state->unicode_bidi == UNICODE_BIDI_NORMAL)
        direction = chunk->writing_direction;

    pango_context_set_language(context, state->lang ? pango_language_from_string(state->lang) : NULL);
    pango_context_set_base_dir(context, direction);
    pango_context_set_base_gravity(context, chunk->gravity);

    PangoFontDescription* font_desc = rsvg_text_font_description(ctx, state, context);
    PangoLayout* layout = pango_layout_new(context);
    pango_layout_set_font_description(layout, font_desc);
    pango_font_description_free(font_desc);

    PangoAttrList* attr_list = pango_attr_list_new();
    PangoAttribute* attribute =
        pango_attr_letter_spacing_new(text_pango_units(_rsvg_css_normalize_length(&state->letter_spacing, ctx, 'h')));
    attribute->start_index = 0;
    attribute->end_index = G_MAXINT;
    pango_attr_list_insert(attr_list, attribute);

    if (state->has_font_decor && text) {
        if (state->font_decor & TEXT_UNDERLINE) {
            attribute = pango_attr_underline_new(PANGO_UNDERLINE_SINGLE);
            attribute->start_index = 0;
            attribute->end_index = -1;
            pango_attr_list_insert(attr_list, attribute);
        }
        if (state->font_decor & TEXT_STRIKE) {
            attribute = pango_attr_strikethrough_new(TRUE);
            attribute->start_index = 0;
            attribute->end_index = -1;
            pango_attr_list_insert(attr_list, attribute);
        }
    }

    pango_layout_set_attributes(layout, attr_list);
    pango_attr_list_unref(attr_list);

    if (text)
        pango_layout_set_text(layout, text, -1);
    else
        pango_layout_set_text(layout, NULL, 0);

    pango_layout_set_alignment(layout, (direction == PANGO_DIRECTION_LTR) ? PANGO_ALIGN_LEFT : PANGO_ALIGN_RIGHT);

    return layout;
}

static RsvgTextLayout* rsvg_text_layout_new(RsvgDrawingCtx* ctx,
                                            RsvgState* state,
                                            const char* text,
                                            const RsvgTextChunk* chunk) {
    RsvgTextLayout* layout;

    if (ctx->pango_context == NULL)
        ctx->pango_context = ctx->render->create_pango_context(ctx);

    layout = g_new0(RsvgTextLayout, 1);

    layout->layout = rsvg_text_create_layout(ctx, state, text, ctx->pango_context, chunk);
    layout->ctx = ctx;

    layout->anchor = state->text_anchor;

    return layout;
}

void rsvg_text_render_text(RsvgDrawingCtx* ctx, const char* text, gdouble* x, gdouble* y, const RsvgTextChunk* chunk) {
    PangoContext* context;
    PangoLayout* layout;
    PangoLayoutIter* iter;
    RsvgState* state;
    gint w, h;
    double offset_x, offset_y, offset;

    state = rsvg_current_state(ctx);

    /* Do not render the text if the font size is zero. See bug #581491. */
    if (text_font_size(ctx, state) == 0.0)
        return;

    context = ctx->render->create_pango_context(ctx);
    layout = rsvg_text_create_layout(ctx, state, text, context, chunk);
    pango_layout_get_size(layout, &w, &h);
    iter = pango_layout_get_iter(layout);
    offset = pango_layout_iter_get_baseline(iter) / (double)PANGO_SCALE;
    offset += rsvg_text_accumulate_baseline_shift(state, ctx, context);
    if (PANGO_GRAVITY_IS_VERTICAL(chunk->gravity)) {
        offset_x = -offset;
        offset_y = 0;
    }
    else {
        offset_x = 0;
        offset_y = offset;
    }
    pango_layout_iter_free(iter);
    if (!PANGO_GRAVITY_IS_VERTICAL(chunk->gravity) && chunk->direction == PANGO_DIRECTION_RTL)
        *x -= w / (double)PANGO_SCALE;
    ctx->render->render_pango_layout(ctx, layout, *x - offset_x, *y - offset_y);
    if (PANGO_GRAVITY_IS_VERTICAL(chunk->gravity))
        *y += w / (double)PANGO_SCALE;
    else if (chunk->direction != PANGO_DIRECTION_RTL)
        *x += w / (double)PANGO_SCALE;

    g_object_unref(layout);
    g_object_unref(context);
}

static gdouble rsvg_text_layout_width(RsvgTextLayout* layout) {
    gint width;

    pango_layout_get_size(layout->layout, &width, NULL);

    return width / (double)PANGO_SCALE;
}

static gdouble rsvg_text_length_text_as_string(RsvgDrawingCtx* ctx, const char* text, const RsvgTextChunk* chunk) {
    RsvgTextLayout* layout;
    gdouble x;

    layout = rsvg_text_layout_new(ctx, rsvg_current_state(ctx), text, chunk);
    layout->x = layout->y = 0;

    x = rsvg_text_layout_width(layout);

    rsvg_text_layout_free(layout);
    return x;
}
