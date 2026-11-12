/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
/* GdkPixbuf library - Simple frame-based animations
 *
 * Copyright (C) Dom Lachowicz
 *
 * Authors: Dom Lachowicz <cinamod@hotmail.com>
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
 *
 * Based on code originally by:
 *          Jonathan Blandford <jrb@redhat.com>
 *          Havoc Pennington <hp@redhat.com>
 */

#include "config.h"
#define GLIB_DISABLE_DEPRECATION_WARNINGS
#include <glib.h>

#define GDK_PIXBUF_C_COMPILATION
#include "gdk-pixbuf.h"
#include "gdk-pixbuf-private.h"
#include "gdk-pixbuf-simple-anim.h"

struct _GdkPixbufSimpleAnimClass {
    GdkPixbufAnimationClass parent_class;
};

/* Private part of the GdkPixbufSimpleAnim structure */
struct _GdkPixbufSimpleAnim {
    GdkPixbufAnimation parent_instance;

    gint delay_time;
    gint total_time;

    GList* frames;

    gint width;
    gint height;

    gboolean loop;
};

typedef struct _GdkPixbufSimpleAnimIter GdkPixbufSimpleAnimIter;
typedef struct _GdkPixbufSimpleAnimIterClass GdkPixbufSimpleAnimIterClass;

#define GDK_TYPE_PIXBUF_SIMPLE_ANIM_ITER (gdk_pixbuf_simple_anim_iter_get_type())
#define GDK_PIXBUF_SIMPLE_ANIM_ITER(object) \
    (G_TYPE_CHECK_INSTANCE_CAST((object), GDK_TYPE_PIXBUF_SIMPLE_ANIM_ITER, GdkPixbufSimpleAnimIter))

GType gdk_pixbuf_simple_anim_iter_get_type(void) G_GNUC_CONST;

struct _GdkPixbufSimpleAnimIterClass {
    GdkPixbufAnimationIterClass parent_class;
};

struct _GdkPixbufSimpleAnimIter {
    GdkPixbufAnimationIter parent_instance;

    GdkPixbufSimpleAnim* simple_anim;

    GTimeVal start_time;
    gint position;

    GList* current_frame;
};

static void gdk_pixbuf_simple_anim_finalize(GObject* object);

static gboolean is_static_image(GdkPixbufAnimation* animation);
static GdkPixbuf* get_static_image(GdkPixbufAnimation* animation);

static void get_size(GdkPixbufAnimation* anim, gint* width, gint* height);
static GdkPixbufAnimationIter* get_iter(GdkPixbufAnimation* anim, const GTimeVal* start_time);

static void gdk_pixbuf_simple_anim_set_property(GObject* object, guint prop_id, const GValue* value, GParamSpec* pspec);
static void gdk_pixbuf_simple_anim_get_property(GObject* object, guint prop_id, GValue* value, GParamSpec* pspec);

enum { PROP_0, PROP_LOOP };

G_DEFINE_TYPE(GdkPixbufSimpleAnim, gdk_pixbuf_simple_anim, GDK_TYPE_PIXBUF_ANIMATION)

static void gdk_pixbuf_simple_anim_init(GdkPixbufSimpleAnim* anim) {}

static void gdk_pixbuf_simple_anim_class_init(GdkPixbufSimpleAnimClass* klass) {
    GObjectClass* object_class;
    GdkPixbufAnimationClass* anim_class;

    object_class = G_OBJECT_CLASS(klass);
    anim_class = GDK_PIXBUF_ANIMATION_CLASS(klass);

    object_class->set_property = gdk_pixbuf_simple_anim_set_property;
    object_class->get_property = gdk_pixbuf_simple_anim_get_property;
    object_class->finalize = gdk_pixbuf_simple_anim_finalize;

    anim_class->is_static_image = is_static_image;
    anim_class->get_static_image = get_static_image;
    anim_class->get_size = get_size;
    anim_class->get_iter = get_iter;

    /**
     * GdkPixbufSimpleAnim:loop:
     *
     * Whether the animation should loop when it reaches the end.
     *
     * Since: 2.18
     */
    g_object_class_install_property(
        object_class, PROP_LOOP,
        g_param_spec_boolean("loop", "Loop", "Whether the animation should loop when it reaches the end", FALSE,
                             G_PARAM_READWRITE));
}

static void gdk_pixbuf_simple_anim_finalize(GObject* object) {
    GdkPixbufSimpleAnim* anim;

    anim = GDK_PIXBUF_SIMPLE_ANIM(object);

    g_list_free_full(anim->frames, g_object_unref);

    G_OBJECT_CLASS(gdk_pixbuf_simple_anim_parent_class)->finalize(object);
}

static gboolean is_static_image(GdkPixbufAnimation* animation) {
    GdkPixbufSimpleAnim* anim;

    anim = GDK_PIXBUF_SIMPLE_ANIM(animation);

    return (anim->frames != NULL && anim->frames->next == NULL);
}

static GdkPixbuf* get_static_image(GdkPixbufAnimation* animation) {
    GdkPixbufSimpleAnim* anim;

    anim = GDK_PIXBUF_SIMPLE_ANIM(animation);

    if (anim->frames == NULL)
        return NULL;
    else
        return anim->frames->data;
}

static void get_size(GdkPixbufAnimation* animation, gint* width, gint* height) {
    GdkPixbufSimpleAnim* anim;

    anim = GDK_PIXBUF_SIMPLE_ANIM(animation);

    if (width)
        *width = anim->width;

    if (height)
        *height = anim->height;
}

static GdkPixbufAnimationIter* get_iter(GdkPixbufAnimation* anim, const GTimeVal* start_time) {
    GdkPixbufSimpleAnimIter* iter;

    iter = g_object_new(GDK_TYPE_PIXBUF_SIMPLE_ANIM_ITER, NULL);

    iter->simple_anim = GDK_PIXBUF_SIMPLE_ANIM(anim);

    g_object_ref(iter->simple_anim);

    iter->current_frame = iter->simple_anim->frames;

    iter->start_time = *start_time;

    return GDK_PIXBUF_ANIMATION_ITER(iter);
}

static void gdk_pixbuf_simple_anim_iter_finalize(GObject* object);

static gint get_delay_time(GdkPixbufAnimationIter* iter);
static GdkPixbuf* get_pixbuf(GdkPixbufAnimationIter* iter);
static gboolean on_currently_loading_frame(GdkPixbufAnimationIter* iter);
static gboolean advance(GdkPixbufAnimationIter* iter, const GTimeVal* current_time);

G_DEFINE_TYPE(GdkPixbufSimpleAnimIter, gdk_pixbuf_simple_anim_iter, GDK_TYPE_PIXBUF_ANIMATION_ITER)

static void gdk_pixbuf_simple_anim_iter_init(GdkPixbufSimpleAnimIter* iter) {}

static void gdk_pixbuf_simple_anim_iter_class_init(GdkPixbufSimpleAnimIterClass* klass) {
    GObjectClass* object_class;
    GdkPixbufAnimationIterClass* anim_iter_class;

    object_class = G_OBJECT_CLASS(klass);
    anim_iter_class = GDK_PIXBUF_ANIMATION_ITER_CLASS(klass);

    object_class->finalize = gdk_pixbuf_simple_anim_iter_finalize;

    anim_iter_class->get_delay_time = get_delay_time;
    anim_iter_class->get_pixbuf = get_pixbuf;
    anim_iter_class->on_currently_loading_frame = on_currently_loading_frame;
    anim_iter_class->advance = advance;
}

static void gdk_pixbuf_simple_anim_iter_finalize(GObject* object) {
    GdkPixbufSimpleAnimIter* iter;

    iter = GDK_PIXBUF_SIMPLE_ANIM_ITER(object);
    iter->current_frame = NULL;

    g_object_unref(iter->simple_anim);

    G_OBJECT_CLASS(gdk_pixbuf_simple_anim_iter_parent_class)->finalize(object);
}

static gboolean advance(GdkPixbufAnimationIter* anim_iter, const GTimeVal* current_time) {
    GdkPixbufSimpleAnimIter* iter;
    gint64 elapsed;
    gint64 loop_count;
    guint64 sec_delta;
    guint64 sec_usec;
    gint64 usec_delta;
    GList* tmp;
    GList* old;

    iter = GDK_PIXBUF_SIMPLE_ANIM_ITER(anim_iter);

    if (iter->simple_anim->frames == NULL)
        return FALSE;

    if (current_time->tv_sec < iter->start_time.tv_sec ||
        (current_time->tv_sec == iter->start_time.tv_sec && current_time->tv_usec < iter->start_time.tv_usec)) {
        /* Try to compensate; probably the system clock
         * was set backwards
         */
        iter->start_time = *current_time;
        elapsed = 0;
    }
    else {
        sec_delta = (guint64)current_time->tv_sec - (guint64)iter->start_time.tv_sec;
        usec_delta = (gint64)current_time->tv_usec - (gint64)iter->start_time.tv_usec;

        if (!g_uint64_checked_mul(&sec_usec, sec_delta, G_USEC_PER_SEC) || sec_usec > (guint64)G_MAXINT64 - 1000000)
            elapsed = G_MAXINT64;
        else
            elapsed = ((gint64)sec_usec + usec_delta) / 1000;
    }

    g_assert(iter->simple_anim->total_time > 0);

    /* See how many times we've already played the full animation,
     * and subtract time for that.
     */
    loop_count = elapsed / iter->simple_anim->total_time;
    elapsed = elapsed % iter->simple_anim->total_time;

    iter->position = (gint)elapsed;

    /* Now move to the proper frame */
    if (loop_count < 1 || iter->simple_anim->loop)
        tmp = g_list_nth(iter->simple_anim->frames, iter->position / iter->simple_anim->delay_time);
    else
        tmp = NULL;

    old = iter->current_frame;

    iter->current_frame = tmp;

    return iter->current_frame != old;
}

static gint get_delay_time(GdkPixbufAnimationIter* anim_iter) {
    GdkPixbufSimpleAnimIter* iter;

    iter = GDK_PIXBUF_SIMPLE_ANIM_ITER(anim_iter);

    if (iter->current_frame) {
        return iter->simple_anim->delay_time - iter->position % iter->simple_anim->delay_time;
    }
    else {
        return -1; /* show last frame forever */
    }
}

static GdkPixbuf* get_pixbuf(GdkPixbufAnimationIter* anim_iter) {
    GdkPixbufSimpleAnimIter* iter;
    GList* frame;

    iter = GDK_PIXBUF_SIMPLE_ANIM_ITER(anim_iter);

    if (iter->current_frame)
        frame = iter->current_frame;
    else
        frame = g_list_last(iter->simple_anim->frames);

    return frame ? frame->data : NULL;
}

static gboolean on_currently_loading_frame(GdkPixbufAnimationIter* anim_iter) {
    GdkPixbufSimpleAnimIter* iter;

    iter = GDK_PIXBUF_SIMPLE_ANIM_ITER(anim_iter);

    return iter->current_frame == NULL || iter->current_frame->next == NULL;
}

/**
 * gdk_pixbuf_simple_anim_new:
 * @width: the width of the animation
 * @height: the height of the animation
 * @rate: the speed of the animation, in frames per second. Must be positive
 *   and finite.
 *
 * Creates a new, empty animation.
 *
 * Frame delays are rounded down to whole milliseconds, with a minimum delay
 * of one millisecond.
 *
 * Returns: (nullable): a newly allocated #GdkPixbufSimpleAnim, or `NULL` if
 *   @rate cannot be represented in milliseconds
 *
 * Since: 2.8
 */
GdkPixbufSimpleAnim* gdk_pixbuf_simple_anim_new(gint width, gint height, gfloat rate) {
    GdkPixbufSimpleAnim* anim;
    double delay;

    g_return_val_if_fail(rate > 0.0f && isfinite(rate), NULL);

    delay = 1000.0 / rate;
    g_return_val_if_fail(delay <= G_MAXINT, NULL);

    anim = g_object_new(GDK_TYPE_PIXBUF_SIMPLE_ANIM, NULL);
    anim->width = width;
    anim->height = height;
    anim->delay_time = MAX(1, (gint)delay);

    return anim;
}

/**
 * gdk_pixbuf_simple_anim_add_frame:
 * @animation: a #GdkPixbufSimpleAnim
 * @pixbuf: the pixbuf to add
 *
 * Adds a new frame to @animation. The @pixbuf must
 * have the dimensions specified when the animation
 * was constructed.
 *
 * Since: 2.8
 */
void gdk_pixbuf_simple_anim_add_frame(GdkPixbufSimpleAnim* animation, GdkPixbuf* pixbuf) {
    g_return_if_fail(GDK_IS_PIXBUF_SIMPLE_ANIM(animation));
    g_return_if_fail(GDK_IS_PIXBUF(pixbuf));
    g_return_if_fail(animation->delay_time > 0);
    g_return_if_fail(animation->total_time <= G_MAXINT - animation->delay_time);

    animation->total_time += animation->delay_time;
    animation->frames = g_list_append(animation->frames, g_object_ref(pixbuf));
}

static void gdk_pixbuf_simple_anim_get_property(GObject* object, guint prop_id, GValue* value, GParamSpec* pspec) {
    GdkPixbufSimpleAnim* animation = GDK_PIXBUF_SIMPLE_ANIM(object);

    switch (prop_id) {
        case PROP_LOOP:
            g_value_set_boolean(value, gdk_pixbuf_simple_anim_get_loop(animation));
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void gdk_pixbuf_simple_anim_set_property(GObject* object,
                                                guint prop_id,
                                                const GValue* value,
                                                GParamSpec* pspec) {
    GdkPixbufSimpleAnim* animation = GDK_PIXBUF_SIMPLE_ANIM(object);

    switch (prop_id) {
        case PROP_LOOP:
            gdk_pixbuf_simple_anim_set_loop(animation, g_value_get_boolean(value));
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

/**
 * gdk_pixbuf_simple_anim_set_loop:
 * @animation: a #GdkPixbufSimpleAnim
 * @loop: whether to loop the animation
 *
 * Sets whether @animation should loop indefinitely when it reaches the end.
 *
 * Since: 2.18
 **/
void gdk_pixbuf_simple_anim_set_loop(GdkPixbufSimpleAnim* animation, gboolean loop) {
    g_return_if_fail(GDK_IS_PIXBUF_SIMPLE_ANIM(animation));

    if (loop != animation->loop) {
        animation->loop = loop;
        g_object_notify(G_OBJECT(animation), "loop");
    }
}

/**
 * gdk_pixbuf_simple_anim_get_loop:
 * @animation: a #GdkPixbufSimpleAnim
 *
 * Gets whether @animation should loop indefinitely when it reaches the end.
 *
 * Returns: %TRUE if the animation loops forever, %FALSE otherwise
 *
 * Since: 2.18
 **/
gboolean gdk_pixbuf_simple_anim_get_loop(GdkPixbufSimpleAnim* animation) {
    g_return_val_if_fail(GDK_IS_PIXBUF_SIMPLE_ANIM(animation), FALSE);

    return animation->loop;
}
