/* Native, consumer-scoped stylesheet adapter. No libcroco objects. */
#include "config.h"
#include "rsvg-css-engine.h"

#include <string.h>
#include "liblexa/css.h"
#include "liblexa/base.h"

#define RSVG_MAX_CSS_AGGREGATE (16u * RSVG_MAX_CSS_SIZE)
#define RSVG_MAX_CSS_IMPORT_DEPTH 8
#define RSVG_MAX_CSS_MEDIA_DEPTH 32

typedef struct {
    RsvgCssEngine *engine;
    GHashTable *props;
    GHashTable *imports; /* active URI keys, not a resource cache */
    lxa_budget_t budget;
    size_t rules, declarations;
    size_t new_user_retained;
    size_t source_order;
    size_t *document_order;
    gboolean document_sheet;
} ParseSession;

typedef struct {
    size_t at, after;
} RuleFrame;

static gboolean parse_sheet(ParseSession *session, const char *source,
                            size_t length, unsigned depth);

static gboolean
at_name(lxa_css_tokens_t *tokens, size_t index, const char *word)
{
    lxa_css_token_t token;
    lxa_span_t text;
    size_t length = strlen(word);
    if (lxa_css_tokens_get(tokens, index, &token, &text) != LXA_OK
        || token.kind != LXA_CSS_AT_KEYWORD || text.length != length)
        return FALSE;
    for (size_t i = 0; i < length; i++) {
        uint8_t c = text.data[i];
        if ((c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c) != word[i])
            return FALSE;
    }
    return TRUE;
}

static gboolean
import_sheet(ParseSession *session, lxa_css_tokens_t *tokens,
             lxa_css_blocks_t *blocks, const lxa_css_rule_t *rule,
             unsigned depth)
{
    lxa_css_import_t event;
    lxa_css_media_t media;
    char *uri, *data, *mime = NULL;
    gsize size = 0;
    gboolean ok;
    if (lxa_css_import_parse(tokens, blocks, rule, &event) != LXA_OK)
        return FALSE;
    if (event.syntax != LXA_CSS_SYNTAX_SUPPORTED) return TRUE;
    if (lxa_css_media_screen(tokens, blocks, event.media_first,
                             event.media_after, &media) != LXA_OK)
        return FALSE;
    if (!media.applies) return TRUE;
    if (depth >= RSVG_MAX_CSS_IMPORT_DEPTH) return FALSE;
    uri = g_strndup((const char *) event.uri.data, event.uri.length);
    if (g_hash_table_lookup(session->imports, uri) != NULL) {
        g_free(uri); /* Cyclic @import has no effect. */
        return TRUE;
    }
    g_hash_table_insert(session->imports, g_strdup(uri),
                        GINT_TO_POINTER(1));
    data = _rsvg_handle_acquire_data(session->engine->ctx, uri, &mime,
                                     &size, NULL);
    if (data == NULL || mime == NULL || strcmp(mime, "text/css") != 0) {
        ok = TRUE; /* Resource policy and MIME checks stay in librsvg. */
    }
    else {
        ok = parse_sheet(session, data, size, depth + 1);
    }
    g_hash_table_remove(session->imports, uri);
    g_free(data);
    g_free(mime);
    g_free(uri);
    return ok;
}

static gboolean
emit_rule(ParseSession *session, lxa_css_tokens_t *tokens,
          lxa_css_blocks_t *blocks, const lxa_css_rule_t *rule,
          lxa_span_t source, const lxa_allocator_t *allocator)
{
    lxa_css_declarations_t *declarations = NULL;
    lxa_css_selector_list_t *selectors = NULL;
    lxa_buffer_t key, value;
    gboolean ok = FALSE;
    if (++session->rules > RSVG_MAX_CSS_RULES) return FALSE;
    if (lxa_css_declarations_create(allocator, blocks, &declarations) != LXA_OK)
        return FALSE;
    if (lxa_css_selector_list_create(allocator, blocks, &selectors) != LXA_OK)
        goto declarations_done;
    if (lxa_buffer_init(&key, allocator) != LXA_OK) goto selectors_done;
    if (lxa_buffer_bind_budget(&key, lxa_css_blocks_budget(blocks)) != LXA_OK)
        goto key_done;
    if (lxa_buffer_init(&value, allocator) != LXA_OK) goto key_done;
    if (lxa_buffer_bind_budget(&value, lxa_css_blocks_budget(blocks)) != LXA_OK)
        goto value_done;
    if (lxa_css_selector_list_build(selectors, tokens, blocks,
            rule->prelude_first, rule->prelude_after, RSVG_MAX_CSS_RULES)
        != LXA_OK) goto value_done;
    if (lxa_css_selector_list_length(selectors) == 0) { ok = TRUE; goto value_done; }
    if (lxa_css_declarations_build(declarations, tokens, blocks,
            rule->block_open + 1, rule->block_after,
            RSVG_MAX_CSS_DECLARATIONS - session->declarations) != LXA_OK)
        goto value_done;
    session->declarations += lxa_css_declarations_length(declarations);
    for (size_t d = 0; d < lxa_css_declarations_length(declarations); d++) {
        lxa_css_declaration_t decl;
        lxa_css_token_t token;
        lxa_span_t name;
        if (lxa_size_add(session->source_order, 1, &session->source_order) != LXA_OK)
            goto value_done;
        if (session->document_order)
            *session->document_order = session->source_order;
        if (lxa_css_declarations_get(declarations, d, &decl) != LXA_OK
            || lxa_css_tokens_get(tokens, decl.name, &token, &name) != LXA_OK
            || name.length == 0 || name.length > RSVG_MAX_CSS_SIZE
            || lxa_css_value_text(tokens, blocks, decl.value_first,
                   decl.value_after, source, RSVG_MAX_CSS_SIZE, &value) != LXA_OK)
            goto value_done;
        for (size_t s = 0; s < lxa_css_selector_list_length(selectors); s++) {
            lxa_css_selector_range_t range;
            lxa_css_syntax_t syntax;
            if (lxa_css_selector_list_get(selectors, s, &range) != LXA_OK
                || lxa_css_selector_key(tokens, blocks, range.first, range.after,
                    RSVG_MAX_CSS_SELECTOR_LENGTH, &key, &syntax) != LXA_OK)
                goto value_done;
            if (syntax != LXA_CSS_SYNTAX_SUPPORTED) continue;
            /* Estimate retained hash/strings overhead before publishing. */
            size_t charge, next_retained;
            size_t current = session->document_sheet ? session->engine->doc_retained : session->new_user_retained;
            if (lxa_size_add(key.length, name.length, &charge) != LXA_OK
                || lxa_size_add(charge, value.length, &charge) != LXA_OK
                || lxa_size_add(charge, 512, &charge) != LXA_OK
                || lxa_size_add(current, charge, &next_retained) != LXA_OK
                || lxa_budget_charge(&session->budget,
                    LXA_BUDGET_MEMORY, charge, NULL) != LXA_OK)
                goto value_done;
            if (session->document_sheet)
                session->engine->doc_retained = next_retained;
            else session->new_user_retained = next_retained;
            char *property = g_strndup((const char *) name.data, name.length);
            rsvg_css_define_style(session->props, (const char *) key.data,
                                  property, (const char *) value.data,
                                  decl.important, session->source_order);
            g_free(property);
        }
    }
    ok = TRUE;
value_done:
    lxa_buffer_destroy(&value);
key_done:
    lxa_buffer_destroy(&key);
selectors_done:
    lxa_css_selector_list_destroy(selectors);
declarations_done:
    lxa_css_declarations_destroy(declarations);
    return ok;
}

static gboolean
walk(ParseSession *session, lxa_css_tokens_t *tokens, lxa_css_blocks_t *blocks,
     lxa_span_t source, unsigned depth, const lxa_allocator_t *allocator)
{
    RuleFrame frames[RSVG_MAX_CSS_MEDIA_DEPTH] = {{0, lxa_css_blocks_end(blocks)}};
    size_t level = 1;
    gboolean past_imports = FALSE;
    while (level) {
        RuleFrame *frame = &frames[level - 1];
        lxa_css_rule_t rule;
        lxa_css_token_t token;
        lxa_span_t text;
        lxa_css_media_t media;
        if (frame->at >= frame->after) { level--; continue; }
        if (lxa_budget_charge(&session->budget, LXA_BUDGET_WORK, 1, NULL)
            != LXA_OK || lxa_css_tokens_get(tokens, frame->at,
                                            &token, &text) != LXA_OK)
            return FALSE;
        if (token.kind == LXA_CSS_WHITESPACE || token.kind == LXA_CSS_CDO
            || token.kind == LXA_CSS_CDC) {
            frame->at++;
            continue;
        }
        if (lxa_css_rule_parse(tokens, blocks, frame->at,
                               frame->after, &rule) != LXA_OK
            || rule.after <= frame->at || rule.after > frame->after)
            return FALSE;
        frame->at = rule.after;
        if (!rule.valid) continue;
        if (rule.kind == LXA_CSS_RULE_AT) {
            if (level == 1 && !past_imports && !rule.has_block
                && at_name(tokens, rule.name, "import")) {
                if (!import_sheet(session, tokens, blocks, &rule, depth))
                    return FALSE;
            }
            else if (rule.has_block && at_name(tokens, rule.name, "media")) {
                past_imports = TRUE;
                if (lxa_css_media_screen(tokens, blocks, rule.prelude_first,
                       rule.prelude_after, &media) != LXA_OK)
                    return FALSE;
                if (media.applies) {
                    if (level == RSVG_MAX_CSS_MEDIA_DEPTH) return FALSE;
                    frames[level++] = (RuleFrame) {rule.block_open + 1,
                                                   rule.block_after};
                }
            }
            else past_imports = TRUE; /* Unknown at-rule stays opaque. */
        }
        else {
            past_imports = TRUE;
            if (rule.has_block && !emit_rule(session, tokens, blocks,
                                              &rule, source, allocator))
                return FALSE;
        }
    }
    return TRUE;
}

static gboolean
parse_sheet(ParseSession *session, const char *source, size_t length,
            unsigned depth)
{
    lxa_allocator_t allocator = session->engine->allocator;
    lxa_css_byte_stream_t *stream = NULL;
    lxa_css_tokens_t *tokens = NULL;
    lxa_css_blocks_t *blocks = NULL;
    size_t consumed;
    gboolean ok = FALSE;
    if (length > RSVG_MAX_CSS_SIZE || length > G_MAXSSIZE
        || (length && (source == NULL || !g_utf8_validate(source, length, NULL))))
        return FALSE;
    if (lxa_css_tokens_create(&allocator, &session->budget, &tokens) != LXA_OK)
        return FALSE;
    if (lxa_css_byte_stream_create_budget(&allocator, &session->budget,
                                           &stream) != LXA_CSS_SCAN_OK)
        goto tokens_done;
    if (lxa_css_byte_stream_feed(stream,
            (lxa_span_t) {(const uint8_t *) source, length}, &consumed)
        != LXA_CSS_SCAN_OK || consumed != length
        || lxa_css_byte_stream_finish(stream, tokens) != LXA_CSS_SCAN_OK)
        goto stream_done;
    if (lxa_css_blocks_create(&allocator, &session->budget, &blocks) != LXA_OK)
        goto stream_done;
    if (lxa_css_blocks_build(blocks, tokens) == LXA_OK)
        ok = walk(session, tokens, blocks,
                  (lxa_span_t) {(const uint8_t *) source, length}, depth,
                  &allocator);
    lxa_css_blocks_destroy(blocks);
stream_done:
    lxa_css_byte_stream_destroy(stream);
tokens_done:
    lxa_css_tokens_destroy(tokens);
    return ok;
}

gboolean rsvg_css_engine_parse_into_props(RsvgCssEngine* engine,
                                          GHashTable* props,
                                          const char* data,
                                          size_t length,
                                          gboolean emit_warnings) {
    ParseSession session = {0};
    lxa_limits_t limits = lxa_limits_default();
    gboolean ok;
    size_t carried;
    if (!length) {
        if (props != engine->doc_css_props)
            engine->user_retained = 0;
        return TRUE;
    }
    if (length > RSVG_MAX_CSS_SIZE) {
        if (emit_warnings)
            g_warning(_("CSS buffer too large (%lu bytes), ignoring"),
                      (unsigned long) length);
        return FALSE;
    }
    limits.max_input_bytes = RSVG_MAX_CSS_AGGREGATE;
    limits.max_memory_bytes = 64u * 1024u * 1024u;
    if (lxa_budget_init(&session.budget, &limits) != LXA_OK ||
        lxa_size_add(engine->doc_retained, engine->user_retained, &carried) != LXA_OK ||
        lxa_budget_charge(&session.budget, LXA_BUDGET_MEMORY, carried, NULL) != LXA_OK)
        return FALSE;
    session.engine = engine;
    session.document_sheet = props == engine->doc_css_props;
    session.document_order = session.document_sheet ? rsvg_css_engine_document_order(engine) : NULL;
    session.source_order = session.document_order ? *session.document_order : 0;
    session.props = props;
    session.imports = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                            NULL);
    ok = parse_sheet(&session, data, length, 0);
    if (ok && !session.document_sheet)
        engine->user_retained = session.new_user_retained;
    g_hash_table_destroy(session.imports);
    if (!ok && emit_warnings) g_warning("CSS stylesheet could not be parsed within its limits");
    return ok;
}
