/*
   rsvg-css-engine.c: CSS Engine Abstraction

   Copyright (C) 2026 ...
*/

#include "config.h"
#include "rsvg-css-engine.h"
#include "rsvg-styles.h"
#include "rsvg-css.h"

#include <string.h>

/* Keep the engine layout reachable through exported helper types unchanged. */
typedef struct {
    RsvgCssEngine engine;
    size_t document_order;
} RsvgCssEngineStorage;

typedef struct {
    const char* property;
    const StyleValueData* value;
    RsvgStyleOrigin origin;
    guint specificity;
} MatchedStyle;

typedef struct {
    RsvgCssEngine* engine;
    GArray* values;
    GHashTable* selectors;
} StyleMatches;

static gint compare_styles(gconstpointer a, gconstpointer b) {
    const MatchedStyle* left = a;
    const MatchedStyle* right = b;
    if (left->specificity != right->specificity)
        return left->specificity < right->specificity ? -1 : 1;
    if (left->value->source_order != right->value->source_order)
        return left->value->source_order < right->value->source_order ? -1 : 1;
    return 0;
}

static GHashTable* rsvg_css_rules_new(void) {
    return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_hash_table_destroy);
}

static void collect_styles(StyleMatches* matches, const char* target, guint specificity) {
    GHashTable* props[] = {matches->engine->doc_css_props, matches->engine->user_css_props};
    GHashTable* styles[] = {g_hash_table_lookup(props[0], target), g_hash_table_lookup(props[1], target)};
    if (!styles[0] && !styles[1])
        return;

    if (!matches->values) {
        matches->values = g_array_new(FALSE, FALSE, sizeof(MatchedStyle));
        matches->selectors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    }
    if (g_hash_table_contains(matches->selectors, target))
        return;
    g_hash_table_add(matches->selectors, g_strdup(target));
    for (guint origin = 0; origin < G_N_ELEMENTS(styles); origin++) {
        if (!styles[origin])
            continue;
        GHashTableIter iter;
        gpointer property, value;
        g_hash_table_iter_init(&iter, styles[origin]);
        while (g_hash_table_iter_next(&iter, &property, &value)) {
            MatchedStyle match = {property, value, origin, specificity};
            g_array_append_val(matches->values, match);
        }
    }
}

RsvgCssEngine* rsvg_css_engine_new_with_allocator(RsvgHandle* ctx, const lxa_allocator_t* allocator) {
    RsvgCssEngine* engine;
    if (allocator == NULL || allocator->allocate == NULL || allocator->resize == NULL || allocator->release == NULL)
        return NULL;

    RsvgCssEngineStorage* storage = g_new0(RsvgCssEngineStorage, 1);
    engine = &storage->engine;
    engine->allocator = *allocator;
    engine->ctx = ctx;
    engine->doc_css_props = rsvg_css_rules_new();
    engine->user_css_props = rsvg_css_rules_new();
    return engine;
}

size_t* rsvg_css_engine_document_order(RsvgCssEngine* engine) {
    return &((RsvgCssEngineStorage*)engine)->document_order;
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
                           gboolean important,
                           size_t source_order) {
    GHashTable* styles;
    gboolean need_insert = FALSE;
    RsvgLength shift;

    if (g_str_equal(style_name, "baseline-shift") && !rsvg_css_parse_baseline_shift(style_value, &shift))
        return;

    styles = g_hash_table_lookup(css_props, selector);
    if (styles == NULL) {
        styles = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)rsvg_style_value_data_free);
        g_hash_table_insert(css_props, (gpointer)g_strdup(selector), styles);
        need_insert = TRUE;
    }
    else {
        StyleValueData* current_value = g_hash_table_lookup(styles, style_name);
        if (current_value == NULL || !current_value->important || important)
            need_insert = TRUE;
    }

    if (need_insert) {
        StyleValueData* value = rsvg_style_value_data_new(style_value, important);
        value->source_order = source_order;
        g_hash_table_insert(styles, (gpointer)g_strdup(style_name), value);
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
    int i = 0;
    int j = 0;
    char* target = NULL;
    GString* klazz_list = NULL;
    StyleMatches matches = {.engine = engine};

    if (engine == NULL)
        return;

    if (atts != NULL && rsvg_property_bag_size(atts) > 0)
        rsvg_parse_style_pairs(engine->ctx, state, atts);

    /* * */
    collect_styles(&matches, "*", 0);

    /* tag */
    if (tag != NULL)
        collect_styles(&matches, tag, 1);

    if (klass != NULL) {
        i = strlen(klass);
        while (j < i) {
            klazz_list = g_string_new(".");

            while (j < i && g_ascii_isspace(klass[j]))
                j++;

            while (j < i && !g_ascii_isspace(klass[j]))
                g_string_append_c(klazz_list, klass[j++]);

            if (klazz_list->len != 1) {
                collect_styles(&matches, klazz_list->str, 2);
                if (tag != NULL) {
                    target = g_strdup_printf("%s%s", tag, klazz_list->str);
                    collect_styles(&matches, target, 3);
                    g_free(target);
                }
                if (id != NULL) {
                    target = g_strdup_printf("%s#%s", klazz_list->str, id);
                    collect_styles(&matches, target, 6);
                    g_free(target);
                }
                if (tag != NULL && id != NULL) {
                    target = g_strdup_printf("%s%s#%s", tag, klazz_list->str, id);
                    collect_styles(&matches, target, 7);
                    g_free(target);
                }
            }

            g_string_free(klazz_list, TRUE);
        }
    }

    /* #id */
    if (id != NULL) {
        target = g_strdup_printf("#%s", id);
        collect_styles(&matches, target, 4);
        g_free(target);
    }

    /* tag#id */
    if (tag != NULL && id != NULL) {
        target = g_strdup_printf("%s#%s", tag, id);
        collect_styles(&matches, target, 5);
        g_free(target);
    }

    if (matches.values) {
        g_array_sort(matches.values, compare_styles);
        for (guint n = 0; n < matches.values->len; n++) {
            const MatchedStyle* match = &g_array_index(matches.values, MatchedStyle, n);
            rsvg_parse_style_pair(engine->ctx, state, match->property, match->value->value, match->value->important,
                                  match->origin);
        }
        g_array_free(matches.values, TRUE);
        g_hash_table_destroy(matches.selectors);
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
