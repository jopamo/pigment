/*
   rsvg-css-engine.c: CSS Engine Abstraction

   Copyright (C) 2026 ...
*/

#include "config.h"
#include "rsvg-css-engine.h"
#include "rsvg-styles.h"

#include <liblexa/css.h>
#include <string.h>

typedef struct {
    RsvgHandle* ctx;
    RsvgState* state;
    RsvgStyleOrigin origin;
} StylesData;

static void apply_style(gpointer key, gpointer style_value, gpointer user_data) {
    const StyleValueData* value = style_value;
    StylesData* data = (StylesData*)user_data;

    rsvg_parse_style_pair(data->ctx, data->state, key, value->value, value->important, data->origin);
}

static GHashTable* rsvg_css_rules_new(void) {
    return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_hash_table_destroy);
}

static void rsvg_css_apply_styles_from_props(GHashTable* css_props,
                                             RsvgHandle* ctx,
                                             const char* target,
                                             RsvgState* state,
                                             RsvgStyleOrigin origin) {
    GHashTable* styles;
    StylesData data;

    styles = g_hash_table_lookup(css_props, target);
    if (styles == NULL)
        return;

    data.ctx = ctx;
    data.state = state;
    data.origin = origin;
    g_hash_table_foreach(styles, apply_style, &data);
}

static void rsvg_css_engine_lookup_apply_css_style(RsvgCssEngine* engine, const char* target, RsvgState* state) {
    rsvg_css_apply_styles_from_props(engine->doc_css_props, engine->ctx, target, state, RSVG_STYLE_ORIGIN_AUTHOR);
    rsvg_css_apply_styles_from_props(engine->user_css_props, engine->ctx, target, state, RSVG_STYLE_ORIGIN_USER);
}

RsvgCssEngine* rsvg_css_engine_new_with_allocator(RsvgHandle* ctx, const lxa_allocator_t* allocator) {
    RsvgCssEngine* engine;
    if (allocator == NULL || allocator->allocate == NULL || allocator->resize == NULL || allocator->release == NULL)
        return NULL;

    engine = g_new0(RsvgCssEngine, 1);
    engine->allocator = *allocator;
    engine->ctx = ctx;
    engine->doc_css_props = rsvg_css_rules_new();
    engine->user_css_props = rsvg_css_rules_new();
    return engine;
}

void rsvg_css_engine_free(RsvgCssEngine* engine) {
    if (engine == NULL)
        return;
    g_hash_table_destroy(engine->doc_css_props);
    g_hash_table_destroy(engine->user_css_props);
    g_free(engine);
}

void rsvg_css_define_style(GHashTable* css_props,
                           const gchar* selector,
                           const gchar* style_name,
                           const gchar* style_value,
                           gboolean important) {
    GHashTable* styles;
    gboolean need_insert = FALSE;

    styles = g_hash_table_lookup(css_props, selector);
    if (styles == NULL) {
        styles = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)rsvg_style_value_data_free);
        g_hash_table_insert(css_props, (gpointer)g_strdup(selector), styles);
        need_insert = TRUE;
    }
    else {
        StyleValueData* current_value = g_hash_table_lookup(styles, style_name);
        if (current_value == NULL || !current_value->important)
            need_insert = TRUE;
    }

    if (need_insert) {
        g_hash_table_insert(styles, (gpointer)g_strdup(style_name),
                            (gpointer)rsvg_style_value_data_new(style_value, important));
    }
}

RsvgCssEngine* rsvg_css_engine_new(RsvgHandle* ctx) {
    lxa_allocator_t allocator = lxa_allocator_default();
    return rsvg_css_engine_new_with_allocator(ctx, &allocator);
}

gboolean rsvg_css_engine_parse_stylesheet(RsvgCssEngine* engine, const char* data, size_t len) {
    if (engine) {
        return rsvg_css_engine_parse_into_props(engine, engine->doc_css_props, data, len, TRUE);
    }
    return FALSE;
}

gboolean rsvg_css_engine_set_user_stylesheet(RsvgCssEngine* engine, const char* data, size_t len) {
    if (engine) {
        GHashTable* new_props = rsvg_css_rules_new();
        gboolean ok = rsvg_css_engine_parse_into_props(engine, new_props, data, len, FALSE);

        if (!ok) {
            g_hash_table_destroy(new_props);
            return FALSE;
        }

        g_hash_table_destroy(engine->user_css_props);
        engine->user_css_props = new_props;
        return TRUE;
    }
    return FALSE;
}

void rsvg_css_engine_apply_styles(RsvgCssEngine* engine,
                                  RsvgNode* node,
                                  RsvgState* state,
                                  const char* tag,
                                  const char* klass,
                                  const char* id,
                                  RsvgPropertyBag* atts) {
    size_t i = 0;
    size_t j = 0;
    char* target = NULL;
    GString* klazz_list = NULL;

    if (engine == NULL)
        return;

    gboolean tag_unambiguous = tag != NULL && lxa_css_selector_key_names_unambiguous(
                                                  (lxa_span_t){(const uint8_t*)tag, strlen(tag)}, (lxa_span_t){0});

    if (atts != NULL && rsvg_property_bag_size(atts) > 0)
        rsvg_parse_style_pairs(engine->ctx, state, atts);

    /* * */
    rsvg_css_engine_lookup_apply_css_style(engine, "*", state);

    /* tag */
    if (tag_unambiguous)
        rsvg_css_engine_lookup_apply_css_style(engine, tag, state);

    if (klass != NULL) {
        i = strlen(klass);
        while (j < i) {
            klazz_list = g_string_new(".");

            while (j < i && g_ascii_isspace(klass[j]))
                j++;

            while (j < i && !g_ascii_isspace(klass[j]))
                g_string_append_c(klazz_list, klass[j++]);

            if (klazz_list->len != 1 &&
                lxa_css_selector_key_names_unambiguous(
                    (lxa_span_t){0}, (lxa_span_t){(const uint8_t*)klazz_list->str + 1, klazz_list->len - 1})) {
                rsvg_css_engine_lookup_apply_css_style(engine, klazz_list->str, state);
                if (tag_unambiguous) {
                    target = g_strdup_printf("%s%s", tag, klazz_list->str);
                    rsvg_css_engine_lookup_apply_css_style(engine, target, state);
                    g_free(target);
                }
                if (id != NULL) {
                    target = g_strdup_printf("%s#%s", klazz_list->str, id);
                    rsvg_css_engine_lookup_apply_css_style(engine, target, state);
                    g_free(target);
                }
                if (tag_unambiguous && id != NULL) {
                    target = g_strdup_printf("%s%s#%s", tag, klazz_list->str, id);
                    rsvg_css_engine_lookup_apply_css_style(engine, target, state);
                    g_free(target);
                }
            }

            g_string_free(klazz_list, TRUE);
        }
    }

    /* #id */
    if (id != NULL) {
        target = g_strdup_printf("#%s", id);
        rsvg_css_engine_lookup_apply_css_style(engine, target, state);
        g_free(target);
    }

    /* tag#id */
    if (tag_unambiguous && id != NULL) {
        target = g_strdup_printf("%s#%s", tag, id);
        rsvg_css_engine_lookup_apply_css_style(engine, target, state);
        g_free(target);
    }

    if (atts != NULL && rsvg_property_bag_size(atts) > 0) {
        const char* value;

        if ((value = rsvg_property_bag_lookup(atts, "style")) != NULL)
            rsvg_parse_style(engine->ctx, state, value);
        if ((value = rsvg_property_bag_lookup(atts, "transform")) != NULL)
            rsvg_parse_transform_attr(engine->ctx, state, value);
    }
    else if (node != NULL && node->style_attr != NULL) {
        rsvg_parse_style(engine->ctx, state, node->style_attr);
    }
}
