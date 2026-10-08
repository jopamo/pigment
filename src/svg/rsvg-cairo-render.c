/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* vim: set sw=4 sts=4 ts=4 expandtab: */
/*
   rsvg-cairo-render.c: The cairo backend plugin

   Copyright (C) 2005 Dom Lachowicz <cinamod@hotmail.com>
   Caleb Moore <c.moore@student.unsw.edu.au>

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

   Authors: Dom Lachowicz <cinamod@hotmail.com>
   Caleb Moore <c.moore@student.unsw.edu.au>
*/

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include <math.h>
#include <string.h>

#include "rsvg.h"
#include "rsvg-private.h"
#include "rsvg-cairo.h"
#include "rsvg-cairo-draw.h"
#include "rsvg-cairo-render.h"
#include "rsvg-styles.h"
#include "rsvg-structure.h"

static void rsvg_cairo_render_free(RsvgRender* self) {
    RsvgCairoRender* me = RSVG_CAIRO_RENDER(self);

    while (me->cr_stack) {
        cairo_destroy(me->cr);
        me->cr = (cairo_t*)me->cr_stack->data;
        me->cr_stack = g_list_delete_link(me->cr_stack, me->cr_stack);
    }

    while (me->bb_stack) {
        g_free(me->bb_stack->data);
        me->bb_stack = g_list_delete_link(me->bb_stack, me->bb_stack);
    }

    while (me->surfaces_stack) {
        cairo_surface_destroy((cairo_surface_t*)me->surfaces_stack->data);
        me->surfaces_stack = g_list_delete_link(me->surfaces_stack, me->surfaces_stack);
    }

    if (me->font_map_for_testing) {
        g_object_unref(me->font_map_for_testing);
        me->font_map_for_testing = NULL;
    }

    if (me->font_config_for_testing) {
        FcConfigDestroy(me->font_config_for_testing);
        me->font_config_for_testing = NULL;
    }

    g_free(me);
}

RsvgCairoRender* rsvg_cairo_render_new(cairo_t* cr, double width, double height) {
    RsvgCairoRender* cairo_render = g_new0(RsvgCairoRender, 1);
    cairo_matrix_t matrix;

    cairo_render->super.type = RSVG_RENDER_TYPE_CAIRO;
    cairo_render->super.free = rsvg_cairo_render_free;
    cairo_render->super.create_pango_context = rsvg_cairo_create_pango_context;
    cairo_render->super.render_pango_layout = rsvg_cairo_render_pango_layout;
    cairo_render->super.render_surface = rsvg_cairo_render_surface;
    cairo_render->super.render_path = rsvg_cairo_render_path;
    cairo_render->super.pop_discrete_layer = rsvg_cairo_pop_discrete_layer;
    cairo_render->super.push_discrete_layer = rsvg_cairo_push_discrete_layer;
    cairo_render->super.add_clipping_rect = rsvg_cairo_add_clipping_rect;
    cairo_render->super.get_surface_of_node = rsvg_cairo_get_surface_of_node;
    cairo_render->width = width;
    cairo_render->height = height;
    cairo_render->offset_x = 0;
    cairo_render->offset_y = 0;
    cairo_render->initial_cr = cr;
    cairo_render->cr = cr;
    cairo_render->cr_stack = NULL;
    cairo_render->bb_stack = NULL;
    cairo_render->surfaces_stack = NULL;

    cairo_render->font_config_for_testing = NULL;
    cairo_render->font_map_for_testing = NULL;

    cairo_matrix_init_identity(&matrix);
    rsvg_bbox_init(&cairo_render->bounds.ink, &matrix);
    rsvg_bbox_init(&cairo_render->bounds.logical, &matrix);

    return cairo_render;
}

static void rsvg_cairo_transformed_image_bounding_box(cairo_matrix_t* transform,
                                                      double width,
                                                      double height,
                                                      double* x0,
                                                      double* y0,
                                                      double* x1,
                                                      double* y1) {
    double x00 = 0, x01 = 0, x10 = width, x11 = width;
    double y00 = 0, y01 = height, y10 = 0, y11 = height;
    double t;

    /* transform the four corners of the image */
    cairo_matrix_transform_point(transform, &x00, &y00);
    cairo_matrix_transform_point(transform, &x01, &y01);
    cairo_matrix_transform_point(transform, &x10, &y10);
    cairo_matrix_transform_point(transform, &x11, &y11);

    /* find minimum and maximum coordinates */
    t = x00 < x01 ? x00 : x01;
    t = t < x10 ? t : x10;
    *x0 = floor(t < x11 ? t : x11);

    t = y00 < y01 ? y00 : y01;
    t = t < y10 ? t : y10;
    *y0 = floor(t < y11 ? t : y11);

    t = x00 > x01 ? x00 : x01;
    t = t > x10 ? t : x10;
    *x1 = ceil(t > x11 ? t : x11);

    t = y00 > y01 ? y00 : y01;
    t = t > y10 ? t : y10;
    *y1 = ceil(t > y11 ? t : y11);
}

RsvgDrawingCtx* rsvg_cairo_new_drawing_ctx(cairo_t* cr, RsvgHandle* handle) {
    RsvgDimensionData data;
    RsvgDrawingCtx* draw;
    RsvgCairoRender* render;
    RsvgState* state;
    cairo_matrix_t affine;
    double bbx0, bby0, bbx1, bby1;

    rsvg_handle_get_dimensions(handle, &data);
    if (data.width == 0 || data.height == 0)
        return NULL;

    draw = g_new(RsvgDrawingCtx, 1);

    cairo_get_matrix(cr, &affine);

    /* find bounding box of image as transformed by the current cairo context
     * The size of this bounding box determines the size of the intermediate
     * surfaces allocated during drawing. */
    rsvg_cairo_transformed_image_bounding_box(&affine, data.width, data.height, &bbx0, &bby0, &bbx1, &bby1);

    render = rsvg_cairo_render_new(cr, bbx1 - bbx0, bby1 - bby0);

    if (!render)
        return NULL;

    draw->render = (RsvgRender*)render;
    render->offset_x = bbx0;
    render->offset_y = bby0;

    draw->state = NULL;

    draw->defs = handle->priv->defs;
    draw->dpi_x = handle->priv->dpi_x;
    draw->dpi_y = handle->priv->dpi_y;
    draw->vb.rect.width = data.em;
    draw->vb.rect.height = data.ex;
    draw->num_elements_acquired = 0;
    draw->pango_context = NULL;
    draw->drawsub_stack = NULL;
    draw->acquired_nodes = NULL;
    draw->is_testing = handle->priv->is_testing;

    rsvg_state_push(draw);
    state = rsvg_current_state(draw);

    /* apply cairo transformation to our affine transform */
    cairo_matrix_multiply(&state->affine, &affine, &state->affine);

    /* scale according to size set by size_func callback */
    cairo_matrix_init_scale(&affine, data.width / data.em, data.height / data.ex);
    cairo_matrix_multiply(&state->affine, &affine, &state->affine);

    /* adjust transform so that the corner of the bounding box above is
     * at (0,0) - we compensate for this in _set_rsvg_affine() in
     * rsvg-cairo-render.c and a few other places */
    state->affine.x0 -= render->offset_x;
    state->affine.y0 -= render->offset_y;

    rsvg_bbox_init(&render->bounds.ink, &state->affine);
    rsvg_bbox_init(&render->bounds.logical, &state->affine);

    return draw;
}

static gboolean rsvg_cairo_check_context(cairo_t* cr, GError** error) {
    cairo_status_t status = cairo_status(cr);
    if (status == CAIRO_STATUS_SUCCESS)
        return TRUE;

    g_set_error(error, RSVG_ERROR, RSVG_ERROR_FAILED, "Cairo context: %s", cairo_status_to_string(status));
    return FALSE;
}

static gboolean rsvg_cairo_check_viewport(const RsvgRectangle* viewport, const char* name, GError** error) {
    if (!viewport ||
        (isfinite(viewport->x) && isfinite(viewport->y) && isfinite(viewport->width) && isfinite(viewport->height)))
        return TRUE;

    g_set_error(error, RSVG_ERROR, RSVG_ERROR_FAILED, "%s is not finite", name);
    return FALSE;
}

static gboolean rsvg_cairo_check_drawing(RsvgDrawingCtx* draw, cairo_t* cr, GError** error) {
    if (rsvg_drawing_ctx_limits_exceeded(draw)) {
        g_set_error_literal(error, RSVG_ERROR, RSVG_ERROR_FAILED, "SVG drawing limits exceeded");
        return FALSE;
    }

    return rsvg_cairo_check_context(cr, error);
}

static gboolean rsvg_handle_render_cairo_node(RsvgHandle* handle, cairo_t* cr, RsvgNode* drawsub, GError** error) {
    RsvgDrawingCtx* draw;
    gboolean retval = FALSE;

    if (!rsvg_cairo_check_context(cr, error))
        return FALSE;

    if (handle->priv->state != RSVG_HANDLE_STATE_CLOSED_OK) {
        g_set_error_literal(error, RSVG_ERROR, RSVG_ERROR_FAILED, "SVG is not fully loaded");
        return FALSE;
    }

    draw = rsvg_cairo_new_drawing_ctx(cr, handle);
    if (!draw) {
        g_set_error_literal(error, RSVG_ERROR, RSVG_ERROR_FAILED, "SVG has invalid intrinsic size");
        return FALSE;
    }

    while (drawsub != NULL) {
        draw->drawsub_stack = g_slist_prepend(draw->drawsub_stack, drawsub);
        drawsub = drawsub->parent;
    }

    rsvg_state_push(draw);
    cairo_save(cr);

    rsvg_node_draw((RsvgNode*)handle->priv->treebase, draw, 0);

    retval = rsvg_cairo_check_drawing(draw, cr, error);

    cairo_restore(cr);
    rsvg_state_pop(draw);
    rsvg_drawing_ctx_free(draw);

    return retval;
}

/**
 * rsvg_handle_render_cairo_sub:
 * @handle: A #RsvgHandle
 * @cr: A Cairo renderer
 * @id: (nullable): An element's id within the SVG, or %NULL to render
 *   the whole SVG. For example, if you have a layer called "layer1"
 *   that you wish to render, pass "##layer1" as the id.
 *
 * Draws a subset of a SVG to a Cairo surface
 *
 * Returns: %TRUE if drawing succeeded.
 *
 * Since: 2.14
 */
gboolean rsvg_handle_render_cairo_sub(RsvgHandle* handle, cairo_t* cr, const char* id) {
    RsvgNode* drawsub = NULL;

    g_return_val_if_fail(handle != NULL, FALSE);
    g_return_val_if_fail(cr != NULL, FALSE);

    if (handle->priv->state != RSVG_HANDLE_STATE_CLOSED_OK)
        return FALSE;

    if (!rsvg_cairo_check_context(cr, NULL))
        return FALSE;

    if (id && *id)
        drawsub = rsvg_defs_lookup(handle->priv->defs, id);

    if (drawsub == NULL && id != NULL)
        return FALSE;

    return rsvg_handle_render_cairo_node(handle, cr, drawsub, NULL);
}

/**
 * rsvg_handle_render_cairo:
 * @handle: A #RsvgHandle
 * @cr: A Cairo renderer
 *
 * Draws a SVG to a Cairo surface
 *
 * Returns: %TRUE if drawing succeeded.
 * Since: 2.14
 */
gboolean rsvg_handle_render_cairo(RsvgHandle* handle, cairo_t* cr) {
    return rsvg_handle_render_cairo_sub(handle, cr, NULL);
}

/**
 * rsvg_handle_render_document:
 * @handle: A #RsvgHandle
 * @cr: A Cairo renderer
 * @viewport: (nullable): viewport to render into, in pixels
 * @error: (out) (optional): return location for a #GError, or %NULL
 *
 * Draws an SVG to a Cairo surface, scaled into the provided viewport.
 *
 * Returns: %TRUE if drawing succeeded.
 */
gboolean rsvg_handle_render_document(RsvgHandle* handle, cairo_t* cr, const RsvgRectangle* viewport, GError** error) {
    RsvgDimensionData dimensions;
    gdouble intrinsic_width = 0.0;
    gdouble intrinsic_height = 0.0;
    gboolean retval;

    g_return_val_if_fail(handle != NULL, FALSE);
    g_return_val_if_fail(cr != NULL, FALSE);

    if (!rsvg_cairo_check_context(cr, error))
        return FALSE;

    if (!rsvg_cairo_check_viewport(viewport, "Viewport", error))
        return FALSE;

    if (!viewport)
        return rsvg_handle_render_cairo_node(handle, cr, NULL, error);

    if (!rsvg_handle_get_intrinsic_size_in_pixels(handle, &intrinsic_width, &intrinsic_height)) {
        rsvg_handle_get_dimensions(handle, &dimensions);
        intrinsic_width = dimensions.width;
        intrinsic_height = dimensions.height;
    }

    if (intrinsic_width == 0.0 || intrinsic_height == 0.0) {
        if (error && *error == NULL)
            g_set_error_literal(error, rsvg_error_quark(), RSVG_ERROR_FAILED, "SVG has invalid intrinsic size");
        return FALSE;
    }

    cairo_save(cr);
    cairo_translate(cr, viewport->x, viewport->y);
    cairo_scale(cr, viewport->width / intrinsic_width, viewport->height / intrinsic_height);

    retval = rsvg_handle_render_cairo_node(handle, cr, NULL, error);

    cairo_restore(cr);

    return retval;
}

/**
 * rsvg_handle_render_layer:
 * @handle: a fully loaded #RsvgHandle
 * @cr: a Cairo context
 * @id: (nullable): a document-local element identifier, or %NULL for the whole document
 * @viewport: viewport to render into
 * @error: (nullable): return location for a #GError
 *
 * Renders the selected layer in document coordinates.
 *
 * Returns: %TRUE if rendering succeeded, or %FALSE otherwise
 */
gboolean rsvg_handle_render_layer(RsvgHandle* handle,
                                  cairo_t* cr,
                                  const char* id,
                                  const RsvgRectangle* viewport,
                                  GError** error) {
    RsvgDimensionData dimensions;
    gdouble intrinsic_width = 0.0;
    gdouble intrinsic_height = 0.0;
    gboolean retval;
    RsvgNode* drawsub = NULL;

    g_return_val_if_fail(handle != NULL, FALSE);
    g_return_val_if_fail(cr != NULL, FALSE);

    if (!rsvg_cairo_check_context(cr, error))
        return FALSE;

    if (!rsvg_cairo_check_viewport(viewport, "Viewport", error))
        return FALSE;

    if (id) {
        drawsub = rsvg_defs_lookup_document(handle->priv->defs, id);
        if (!drawsub) {
            if (error && *error == NULL)
                g_set_error_literal(error, rsvg_error_quark(), RSVG_ERROR_FAILED, "Element not found");
            return FALSE;
        }
    }

    if (!viewport)
        return rsvg_handle_render_cairo_node(handle, cr, drawsub, error);

    if (!rsvg_handle_get_intrinsic_size_in_pixels(handle, &intrinsic_width, &intrinsic_height)) {
        rsvg_handle_get_dimensions(handle, &dimensions);
        intrinsic_width = dimensions.width;
        intrinsic_height = dimensions.height;
    }

    if (intrinsic_width == 0.0 || intrinsic_height == 0.0) {
        if (error && *error == NULL)
            g_set_error_literal(error, rsvg_error_quark(), RSVG_ERROR_FAILED, "SVG has invalid intrinsic size");
        return FALSE;
    }

    cairo_save(cr);
    cairo_translate(cr, viewport->x, viewport->y);
    cairo_scale(cr, viewport->width / intrinsic_width, viewport->height / intrinsic_height);

    retval = rsvg_handle_render_cairo_node(handle, cr, drawsub, error);

    cairo_restore(cr);

    return retval;
}

static gboolean rsvg_cairo_draw_element(RsvgHandle* handle,
                                        cairo_t* cr,
                                        RsvgNode* node,
                                        RsvgCairoBounds* bbox,
                                        gboolean measure_logical,
                                        GError** error) {
    if (!rsvg_cairo_check_context(cr, error))
        return FALSE;

    RsvgDrawingCtx* draw = rsvg_cairo_new_drawing_ctx(cr, handle);
    if (!draw) {
        g_set_error_literal(error, RSVG_ERROR, RSVG_ERROR_FAILED, "SVG has invalid intrinsic size");
        return FALSE;
    }
    RSVG_CAIRO_RENDER(draw->render)->measure_logical = measure_logical;
    RSVG_CAIRO_RENDER(draw->render)->measure_ink = bbox != NULL;

    rsvg_state_reconstruct(rsvg_current_state(draw), node->parent);
    rsvg_state_push(draw);
    rsvg_node_draw(node, draw, 0);

    gboolean retval = rsvg_cairo_check_drawing(draw, cr, error);
    if (retval && bbox)
        *bbox = RSVG_CAIRO_RENDER(draw->render)->bounds;

    rsvg_state_pop(draw);
    rsvg_drawing_ctx_free(draw);
    return retval;
}

static gboolean rsvg_cairo_measure_element(RsvgHandle* handle,
                                           RsvgNode* node,
                                           RsvgCairoBounds* bbox,
                                           gboolean measure_logical,
                                           GError** error) {
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t* cr = cairo_create(surface);
    gboolean retval = rsvg_cairo_draw_element(handle, cr, node, bbox, measure_logical, error);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    return retval;
}

/**
 * rsvg_handle_render_element:
 * @handle: a fully loaded #RsvgHandle
 * @cr: a Cairo context
 * @id: (nullable): a document-local element identifier, or %NULL for the whole document
 * @element_viewport: viewport to render into
 * @error: (nullable): return location for a #GError
 *
 * Renders the selected subtree without ancestor transforms, fitting its ink
 * bounds proportionally at the viewport origin.
 *
 * Returns: %TRUE if rendering succeeded, or %FALSE otherwise
 */
gboolean rsvg_handle_render_element(RsvgHandle* handle,
                                    cairo_t* cr,
                                    const char* id,
                                    const RsvgRectangle* element_viewport,
                                    GError** error) {
    RsvgNode* drawsub = NULL;
    RsvgCairoBounds bbox;
    double factor = 1.0;

    g_return_val_if_fail(handle != NULL, FALSE);
    g_return_val_if_fail(cr != NULL, FALSE);

    if (handle->priv->state != RSVG_HANDLE_STATE_CLOSED_OK) {
        g_set_error_literal(error, RSVG_ERROR, RSVG_ERROR_FAILED, "SVG is not fully loaded");
        return FALSE;
    }

    if (!rsvg_cairo_check_context(cr, error))
        return FALSE;

    if (id && *id)
        drawsub = rsvg_defs_lookup_document(handle->priv->defs, id);
    else if (!id)
        drawsub = (RsvgNode*)handle->priv->treebase;

    if (drawsub == NULL) {
        if (error)
            g_set_error_literal(error, rsvg_error_quark(), RSVG_ERROR_FAILED, "Element not found");
        return FALSE;
    }

    if (element_viewport) {
        if (!rsvg_cairo_check_viewport(element_viewport, "Element viewport", error))
            return FALSE;
        if (!rsvg_cairo_measure_element(handle, drawsub, &bbox, FALSE, error))
            return FALSE;
        if (bbox.ink.virgin || bbox.ink.rect.width <= 0 || bbox.ink.rect.height <= 0)
            return TRUE;

        factor = MIN(element_viewport->width / bbox.ink.rect.width, element_viewport->height / bbox.ink.rect.height);
        if (!isfinite(factor) || !isfinite(bbox.ink.rect.x) || !isfinite(bbox.ink.rect.y) ||
            !isfinite(bbox.ink.rect.width) || !isfinite(bbox.ink.rect.height)) {
            g_set_error_literal(error, RSVG_ERROR, RSVG_ERROR_FAILED, "Element geometry or scale is not finite");
            return FALSE;
        }
    }

    cairo_save(cr);
    if (element_viewport) {
        cairo_translate(cr, element_viewport->x, element_viewport->y);
        cairo_scale(cr, factor, factor);
        cairo_translate(cr, -bbox.ink.rect.x, -bbox.ink.rect.y);
    }
    gboolean retval = rsvg_cairo_draw_element(handle, cr, drawsub, NULL, FALSE, error);
    cairo_restore(cr);
    return retval;
}

/**
 * rsvg_handle_get_geometry_for_layer:
 * @handle: a fully loaded #RsvgHandle
 * @id: (nullable): an element identifier, or %NULL for the whole document
 * @viewport: (nullable): the target viewport, or %NULL for document dimensions
 * @out_ink_rect: (out caller-allocates) (optional): location for the ink bounds
 * @out_logical_rect: (out caller-allocates) (optional): location for the logical bounds
 * @error: (nullable): return location for a #GError
 *
 * Queries the geometry of a document or layer. Only read output rectangles
 * when the query succeeds.
 *
 * Returns: %TRUE if the query succeeded, or %FALSE otherwise
 */
gboolean rsvg_handle_get_geometry_for_layer(RsvgHandle* handle,
                                            const char* id,
                                            const RsvgRectangle* viewport,
                                            RsvgRectangle* out_ink_rect,
                                            RsvgRectangle* out_logical_rect,
                                            GError** error) {
    cairo_surface_t* surf;
    cairo_t* cr;
    RsvgDrawingCtx* draw;
    RsvgNode* drawsub = NULL;
    gboolean retval = FALSE;
    RsvgCairoRender* render;

    g_return_val_if_fail(handle != NULL, FALSE);
    if (handle->priv->state != RSVG_HANDLE_STATE_CLOSED_OK) {
        g_set_error_literal(error, RSVG_ERROR, RSVG_ERROR_FAILED, "SVG is not fully loaded");
        return FALSE;
    }
    if (!rsvg_cairo_check_viewport(viewport, "Viewport", error))
        return FALSE;

    if (id)
        drawsub = rsvg_defs_lookup_document(handle->priv->defs, id);
    else
        drawsub = (RsvgNode*)handle->priv->treebase;

    if (!drawsub) {
        if (error)
            g_set_error_literal(error, rsvg_error_quark(), RSVG_ERROR_FAILED, "Element not found");
        return FALSE;
    }

    surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cr = cairo_create(surf);

    if (viewport) {
        RsvgDimensionData dimensions;
        gdouble i_w = 0, i_h = 0;
        if (!rsvg_handle_get_intrinsic_size_in_pixels(handle, &i_w, &i_h)) {
            rsvg_handle_get_dimensions(handle, &dimensions);
            i_w = dimensions.width;
            i_h = dimensions.height;
        }
        if (i_w > 0 && i_h > 0) {
            cairo_translate(cr, viewport->x, viewport->y);
            cairo_scale(cr, viewport->width / i_w, viewport->height / i_h);
        }
    }

    draw = rsvg_cairo_new_drawing_ctx(cr, handle);
    if (!draw) {
        g_set_error_literal(error, RSVG_ERROR, RSVG_ERROR_FAILED, "SVG has invalid intrinsic size");
        cairo_destroy(cr);
        cairo_surface_destroy(surf);
        return FALSE;
    }
    render = RSVG_CAIRO_RENDER(draw->render);
    render->measure_ink = out_ink_rect != NULL || out_logical_rect != NULL;
    render->measure_logical = out_logical_rect != NULL;

    if (id && *id) {
        for (RsvgNode* p = drawsub; p; p = p->parent)
            draw->drawsub_stack = g_slist_prepend(draw->drawsub_stack, p);
    }

    rsvg_state_push(draw);
    rsvg_node_draw((RsvgNode*)handle->priv->treebase, draw, 0);

    retval = rsvg_cairo_check_drawing(draw, cr, error);
    if (retval && out_ink_rect) {
        out_ink_rect->x = render->bounds.ink.rect.x;
        out_ink_rect->y = render->bounds.ink.rect.y;
        out_ink_rect->width = render->bounds.ink.rect.width;
        out_ink_rect->height = render->bounds.ink.rect.height;
    }
    if (retval && out_logical_rect) {
        out_logical_rect->x = render->bounds.logical.rect.x;
        out_logical_rect->y = render->bounds.logical.rect.y;
        out_logical_rect->width = render->bounds.logical.rect.width;
        out_logical_rect->height = render->bounds.logical.rect.height;
    }

    rsvg_state_pop(draw);
    rsvg_drawing_ctx_free(draw);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);

    return retval;
}

/**
 * rsvg_handle_get_geometry_for_element:
 * @handle: a fully loaded #RsvgHandle
 * @id: (nullable): an element identifier, or %NULL for the whole document
 * @out_ink_rect: (out caller-allocates) (optional): location for the ink bounds
 * @out_logical_rect: (out caller-allocates) (optional): location for the logical bounds
 * @error: (nullable): return location for a #GError
 *
 * Queries the geometry of an element without ancestor transforms, with bounds
 * relative to the ink origin. Only read output rectangles when the query succeeds.
 *
 * Returns: %TRUE if the query succeeded, or %FALSE otherwise
 */
gboolean rsvg_handle_get_geometry_for_element(RsvgHandle* handle,
                                              const char* id,
                                              RsvgRectangle* out_ink_rect,
                                              RsvgRectangle* out_logical_rect,
                                              GError** error) {
    RsvgNode* drawsub = NULL;
    RsvgCairoBounds bbox;

    if (id && *id)
        drawsub = rsvg_defs_lookup_document(handle->priv->defs, id);
    else if (!id)
        drawsub = (RsvgNode*)handle->priv->treebase;

    if (!drawsub) {
        if (error)
            g_set_error_literal(error, rsvg_error_quark(), RSVG_ERROR_FAILED, "Element not found");
        return FALSE;
    }

    if (!rsvg_cairo_measure_element(handle, drawsub, &bbox, out_logical_rect != NULL, error))
        return FALSE;

    if (out_ink_rect) {
        out_ink_rect->x = 0;
        out_ink_rect->y = 0;
        out_ink_rect->width = bbox.ink.rect.width;
        out_ink_rect->height = bbox.ink.rect.height;
    }
    if (out_logical_rect) {
        out_logical_rect->x = bbox.logical.rect.x - bbox.ink.rect.x;
        out_logical_rect->y = bbox.logical.rect.y - bbox.ink.rect.y;
        out_logical_rect->width = bbox.logical.rect.width;
        out_logical_rect->height = bbox.logical.rect.height;
    }

    return TRUE;
}
