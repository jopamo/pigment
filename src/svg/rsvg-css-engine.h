/*
   rsvg-css-engine.h: CSS Engine Abstraction

   Copyright (C) 2026 ...
*/

#ifndef RSVG_CSS_ENGINE_H
#define RSVG_CSS_ENGINE_H

#include <glib.h>
#include <glib-object.h>
#include <liblexa/base.h>
#include "rsvg-private.h"

G_BEGIN_DECLS

struct _RsvgCssEngine {
    RsvgHandle* ctx;
    GHashTable* doc_css_props;
    GHashTable* user_css_props;
    /* Conservative retained-style charges across document style elements.
       A replacement sheet is charged while the old user sheet is still live. */
    size_t doc_retained, user_retained;
    lxa_allocator_t allocator; /* fixed before the first parser allocation */
};

#define RSVG_MAX_CSS_SIZE (1024 * 1024) /* 1MB */
#define RSVG_MAX_CSS_RULES 5000
#define RSVG_MAX_CSS_DECLARATIONS 50000
#define RSVG_MAX_CSS_SELECTOR_LENGTH 512

G_GNUC_INTERNAL
gboolean rsvg_css_engine_parse_into_props(RsvgCssEngine* engine,
                                          GHashTable* props,
                                          const char* data,
                                          size_t len,
                                          gboolean emit_warnings);

G_GNUC_INTERNAL
void rsvg_css_define_style(GHashTable* css_props,
                           const gchar* selector,
                           const gchar* style_name,
                           const gchar* style_value,
                           gboolean important);

G_GNUC_INTERNAL
void rsvg_css_engine_free(RsvgCssEngine* engine);

G_GNUC_INTERNAL
gboolean rsvg_css_engine_parse_stylesheet(RsvgCssEngine* engine, const char* data, size_t len);

G_GNUC_INTERNAL
gboolean rsvg_css_engine_set_user_stylesheet(RsvgCssEngine* engine, const char* data, size_t len);

G_GNUC_INTERNAL
void rsvg_css_engine_apply_styles(RsvgCssEngine* engine,
                                  RsvgNode* node,
                                  RsvgState* state,
                                  const char* tag,
                                  const char* klass,
                                  const char* id,
                                  RsvgPropertyBag* atts);

G_GNUC_INTERNAL
RsvgCssEngine* rsvg_css_engine_new(RsvgHandle* ctx);

/* Internal constructor for deterministic allocator-failure qualification.
   The borrowed allocator context outlives the returned engine. */
G_GNUC_INTERNAL
RsvgCssEngine* rsvg_css_engine_new_with_allocator(RsvgHandle* ctx, const lxa_allocator_t* allocator);

G_END_DECLS

#endif /* RSVG_CSS_ENGINE_H */
