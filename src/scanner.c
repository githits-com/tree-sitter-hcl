#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "tree_sitter/parser.h"
#include "identifier_ranges.h"
#include "heredoc_hash.h"
#include <wctype.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define VEC_RESIZE(vec, _cap)                                                  \
    {                                                                          \
        void *tmp = realloc((vec).data, (_cap) * sizeof((vec).data[0]));       \
        assert(tmp != NULL);                                                   \
        (vec).data = tmp;                                                      \
        assert((vec).data != NULL);                                            \
        (vec).cap = (_cap);                                                    \
    }

#define VEC_PUSH(vec, el)                                                      \
    {                                                                          \
        if ((vec).cap == (vec).len) {                                          \
            VEC_RESIZE((vec), MAX(16, (vec).len * 2));                         \
        }                                                                      \
        (vec).data[(vec).len++] = (el);                                        \
    }

#define VEC_POP(vec)                                                           \
    {                                                                          \
        STRING_FREE(VEC_BACK((vec)).heredoc_identifier);                       \
        (vec).len--;                                                           \
    }

#define VEC_BACK(vec) ((vec).data[(vec).len - 1])

#define VEC_FREE(vec)                                                          \
    {                                                                          \
        if ((vec).data != NULL)                                                \
            free((vec).data);                                                  \
        (vec).data = NULL;                                                     \
    }

#define VEC_CLEAR(vec)                                                         \
    {                                                                          \
        for (uint32_t i = 0; i < (vec).len; i++) {                             \
            STRING_FREE((vec).data[i].heredoc_identifier);                     \
        }                                                                      \
        (vec).len = 0;                                                         \
    }

#define STRING_RESIZE(vec, _cap)                                               \
    {                                                                          \
        void *tmp = realloc((vec).data, ((_cap) + 1) * sizeof((vec).data[0])); \
        assert(tmp != NULL);                                                   \
        (vec).data = tmp;                                                      \
        memset((vec).data + (vec).len, 0,                                      \
               (((_cap) + 1) - (vec).len) * sizeof((vec).data[0]));            \
        (vec).cap = (_cap);                                                    \
    }

#define STRING_GROW(vec, _cap)                                                 \
    if ((vec).cap < (_cap)) {                                                  \
        STRING_RESIZE((vec), (_cap));                                          \
    }

#define STRING_PUSH(vec, el)                                                   \
    if ((vec).cap == (vec).len) {                                              \
        STRING_RESIZE((vec), MAX(16, (vec).len * 2));                          \
    }                                                                          \
    (vec).data[(vec).len++] = (el);

#define STRING_FREE(vec)                                                       \
    if ((vec).data != NULL)                                                    \
        free((vec).data);                                                      \
    (vec).data = NULL;

enum TokenType {
    QUOTED_TEMPLATE_START,
    QUOTED_TEMPLATE_END,
    TEMPLATE_LITERAL_CHUNK,
    TEMPLATE_INTERPOLATION_START,
    TEMPLATE_INTERPOLATION_END,
    TEMPLATE_DIRECTIVE_START,
    TEMPLATE_DIRECTIVE_END,
    HEREDOC_IDENTIFIER,
};

typedef enum ContextType {
    TEMPLATE_INTERPOLATION,
    TEMPLATE_DIRECTIVE,
    QUOTED_TEMPLATE,
    HEREDOC_TEMPLATE,
} ContextType;

typedef struct {
    uint32_t cap;
    uint32_t len;
    char *data;
} String;

static String string_new() { return (String){.cap = 16, .len = 0, .data = calloc(1, sizeof(char) * 17)}; }

typedef struct {
    ContextType type;

    // Original UTF-8 length; keep it before String to use existing 64-bit
    // alignment padding. Contexts are copied on every external token.
    uint32_t heredoc_identifier_length;
    // Valid if type == HEREDOC_TEMPLATE; long payloads contain only a digest.
    String heredoc_identifier;
} Context;

typedef struct {
    uint32_t len;
    uint32_t cap;
    Context *data;
} context_vec;

typedef struct {
    context_vec context_stack;
} Scanner;

static inline void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

static inline void skip(TSLexer *lexer) { lexer->advance(lexer, true); }

// Scanner lookahead is a Unicode codepoint; persisted identifiers are UTF-8.
static unsigned encode_codepoint(int32_t codepoint, char bytes[4]) {
    if (codepoint <= 0x7f) {
        bytes[0] = (char)codepoint;
        return 1;
    }
    if (codepoint <= 0x7ff) {
        bytes[0] = (char)(0xc0 | (codepoint >> 6));
        bytes[1] = (char)(0x80 | (codepoint & 0x3f));
        return 2;
    }
    if (codepoint <= 0xffff) {
        bytes[0] = (char)(0xe0 | (codepoint >> 12));
        bytes[1] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        bytes[2] = (char)(0x80 | (codepoint & 0x3f));
        return 3;
    }
    bytes[0] = (char)(0xf0 | (codepoint >> 18));
    bytes[1] = (char)(0x80 | ((codepoint >> 12) & 0x3f));
    bytes[2] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
    bytes[3] = (char)(0x80 | (codepoint & 0x3f));
    return 4;
}

static bool identifier_start(int32_t codepoint) {
    return set_contains(hcl_identifier_start,
        sizeof(hcl_identifier_start) / sizeof(hcl_identifier_start[0]), codepoint);
}

static bool identifier_continue(int32_t codepoint) {
    // The grammar also permits :: in function names; heredoc identifiers do not.
    return codepoint != ':' && set_contains(hcl_identifier_continue,
        sizeof(hcl_identifier_continue) / sizeof(hcl_identifier_continue[0]), codepoint);
}

static unsigned serialize(Scanner *scanner, char *buf) {
    unsigned size = 0;

    memcpy(&buf[size], &(scanner->context_stack.len), sizeof(uint32_t));
    size += sizeof(uint32_t);
    for (uint32_t i = 0; i < scanner->context_stack.len; i++) {
        Context *context = &scanner->context_stack.data[i];
        if (size + sizeof(ContextType) + sizeof(uint32_t) + context->heredoc_identifier.len >= TREE_SITTER_SERIALIZATION_BUFFER_SIZE) {
            return 0;
        }
        memcpy(&buf[size], &(context->type), sizeof(ContextType));
        size += sizeof(ContextType);
        memcpy(&buf[size], &(context->heredoc_identifier_length), sizeof(uint32_t));
        size += sizeof(uint32_t);
        memcpy(&buf[size], context->heredoc_identifier.data, context->heredoc_identifier.len);
        size += context->heredoc_identifier.len;
    }
    return size;
}

static void deserialize(Scanner *scanner, const char *buffer, unsigned length) {
    VEC_CLEAR(scanner->context_stack);

    if (length == 0) {
        return;
    }

    unsigned size = 0;
    uint32_t context_stack_size;
    memcpy(&context_stack_size, &buffer[size], sizeof(uint32_t));
    size += sizeof(uint32_t);
    for (uint32_t j = 0; j < context_stack_size; j++) {
        Context ctx = {0};
        ctx.heredoc_identifier = string_new();

        memcpy(&(ctx.type), &buffer[size], sizeof(ContextType));
        size += sizeof(ContextType);

        memcpy(&ctx.heredoc_identifier_length, &buffer[size], sizeof(uint32_t));
        size += sizeof(uint32_t);
        uint32_t heredoc_identifier_size = ctx.heredoc_identifier_length > HCL_HEREDOC_HASH_SIZE
            ? HCL_HEREDOC_HASH_SIZE : ctx.heredoc_identifier_length;
        if (heredoc_identifier_size > 0) {
          STRING_GROW(ctx.heredoc_identifier, heredoc_identifier_size);
          memcpy(ctx.heredoc_identifier.data, buffer + size, heredoc_identifier_size);
          ctx.heredoc_identifier.len = heredoc_identifier_size;
          size += heredoc_identifier_size;
        }
        VEC_PUSH(scanner->context_stack, ctx);
    }
    assert(size == length);
}

static inline bool accept_inplace(TSLexer *lexer, enum TokenType token) {
    lexer->result_symbol = token;
    return true;
}

static inline bool accept_and_advance(TSLexer *lexer, enum TokenType token) {
    advance(lexer);
    return accept_inplace(lexer, token);
}

static inline bool consume_wxdigit(TSLexer *lexer) {
    advance(lexer);
    return iswxdigit(lexer->lookahead);
}

static inline bool in_context_type(Scanner *scanner, enum ContextType type) {
    if (scanner->context_stack.len == 0) {
        return false;
    }
    return VEC_BACK(scanner->context_stack).type == type;
}

static inline bool in_quoted_context(Scanner *scanner) { return in_context_type(scanner, QUOTED_TEMPLATE); }

static inline bool in_heredoc_context(Scanner *scanner) { return in_context_type(scanner, HEREDOC_TEMPLATE); }

static inline bool in_template_context(Scanner *scanner) {
    return in_quoted_context(scanner) || in_heredoc_context(scanner);
}

static inline bool in_interpolation_context(Scanner *scanner) {
    return in_context_type(scanner, TEMPLATE_INTERPOLATION);
}

static inline bool in_directive_context(Scanner *scanner) { return in_context_type(scanner, TEMPLATE_DIRECTIVE); }

// Preserve the existing escape grammar while allowing a whole literal run.
static bool scan_quoted_escape(TSLexer *lexer) {
    advance(lexer);
    switch (lexer->lookahead) {
        case '"':
        case 'n':
        case 'r':
        case 't':
        case '\\':
            advance(lexer);
            return true;
        case 'u':
        case 'U': {
            unsigned digits = lexer->lookahead == 'u' ? 4 : 8;
            for (unsigned i = 0; i < digits; i++) {
                if (!consume_wxdigit(lexer)) return false;
            }
            advance(lexer);
            return true;
        }
        default:
            return false;
    }
}

// Leave the first mismatching codepoint available to literal/marker handling.
static bool match_heredoc_prefix(const Context *context, TSLexer *lexer, bool *consumed) {
    const String *identifier = &context->heredoc_identifier;
    *consumed = false;
    if (context->heredoc_identifier_length > HCL_HEREDOC_HASH_SIZE) {
        // Openers keep the existing serialization-sized limit. A candidate can
        // therefore be compared on the stack without allocation per body line.
        char candidate[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
        uint32_t size = 0;
        while (size < context->heredoc_identifier_length && identifier_continue(lexer->lookahead)) {
            char bytes[4];
            unsigned length = encode_codepoint(lexer->lookahead, bytes);
            if (length > context->heredoc_identifier_length - size) return false;
            memcpy(candidate + size, bytes, length);
            advance(lexer);
            *consumed = true;
            size += length;
        }
        if (size != context->heredoc_identifier_length) return false;
        uint8_t digest[HCL_HEREDOC_HASH_SIZE];
        hcl_heredoc_hash(digest, candidate, size);
        return memcmp(identifier->data, digest, HCL_HEREDOC_HASH_SIZE) == 0;
    }
    for (size_t i = 0; i < identifier->len;) {
        char bytes[4];
        unsigned length = encode_codepoint(lexer->lookahead, bytes);
        if (length > identifier->len - i ||
            memcmp(identifier->data + i, bytes, length) != 0) return false;
        advance(lexer);
        *consumed = true;
        i += length;
    }
    return true;
}

// The caller has already marked a nonempty literal prefix. Lookahead may pass
// its end, but only completed literal units extend the accepted token range.
static bool scan_literal_tail(Scanner *scanner, TSLexer *lexer, const bool *valid_symbols) {
    // Only the generated recovery set enables both quoted-end and heredoc-id.
    // Preserve its token granularity instead of batching a speculative context.
    if (valid_symbols[QUOTED_TEMPLATE_END] && valid_symbols[HEREDOC_IDENTIFIER]) {
        return accept_inplace(lexer, TEMPLATE_LITERAL_CHUNK);
    }
    bool line_start = false;
    while (lexer->lookahead != 0) {
        if (iswspace(lexer->lookahead)) {
            if (lexer->lookahead == '\n') line_start = true;
            advance(lexer);
            continue;
        }
        if (line_start && in_heredoc_context(scanner) && valid_symbols[HEREDOC_IDENTIFIER]) {
            bool consumed;
            if (match_heredoc_prefix(&VEC_BACK(scanner->context_stack), lexer, &consumed)) {
                // The next scan decides whether this is an own-line closer.
                // Keep this token's old end and the heredoc context intact.
                return accept_inplace(lexer, TEMPLATE_LITERAL_CHUNK);
            }
            if (consumed) lexer->mark_end(lexer);
            line_start = false;
            continue;
        }
        line_start = false;
        if (in_quoted_context(scanner)) {
            if (valid_symbols[QUOTED_TEMPLATE_END] && lexer->lookahead == '"') break;
            if (lexer->lookahead == '\\') {
                if (!scan_quoted_escape(lexer)) break;
                lexer->mark_end(lexer);
                continue;
            }
        }
        bool interpolation = lexer->lookahead == '$' && valid_symbols[TEMPLATE_INTERPOLATION_START];
        bool directive = lexer->lookahead == '%' && valid_symbols[TEMPLATE_DIRECTIVE_START];
        if (interpolation || directive) {
            int32_t sigil = lexer->lookahead;
            advance(lexer);
            if (lexer->lookahead == '{') break;
            if (lexer->lookahead == sigil) {
                advance(lexer);
                if (lexer->lookahead == '{') advance(lexer);
            }
        } else {
            advance(lexer);
        }
        lexer->mark_end(lexer);
    }
    return accept_inplace(lexer, TEMPLATE_LITERAL_CHUNK);
}

static bool finish_literal(Scanner *scanner, TSLexer *lexer, const bool *valid_symbols) {
    lexer->mark_end(lexer);
    if (in_template_context(scanner)) return scan_literal_tail(scanner, lexer, valid_symbols);
    return accept_inplace(lexer, TEMPLATE_LITERAL_CHUNK);
}

static bool scan(Scanner *scanner, TSLexer *lexer, const bool *valid_symbols) {
    bool has_leading_whitespace_with_newline = false;
    while (iswspace(lexer->lookahead)) {
        if (lexer->lookahead == '\n') {
            has_leading_whitespace_with_newline = true;
        }
        skip(lexer);
    }
    if (lexer->lookahead == '\0') {
        return false;
    }
    // manage quoted context
    if (valid_symbols[QUOTED_TEMPLATE_START] && !in_quoted_context(scanner) && lexer->lookahead == '"') {
        Context ctx = {0};
        ctx.type = QUOTED_TEMPLATE;
        ctx.heredoc_identifier = string_new();
        VEC_PUSH(scanner->context_stack, ctx);
        return accept_and_advance(lexer, QUOTED_TEMPLATE_START);
    }
    if (valid_symbols[QUOTED_TEMPLATE_END] && in_quoted_context(scanner) && lexer->lookahead == '"') {
        VEC_POP(scanner->context_stack);
        return accept_and_advance(lexer, QUOTED_TEMPLATE_END);
    }

    // manage template interpolations
    if (valid_symbols[TEMPLATE_INTERPOLATION_START] && valid_symbols[TEMPLATE_LITERAL_CHUNK] &&
        !in_interpolation_context(scanner) && lexer->lookahead == '$') {
        advance(lexer);
        if (lexer->lookahead == '{') {
            Context ctx = {0};
            ctx.type = TEMPLATE_INTERPOLATION;
            ctx.heredoc_identifier = string_new();
            VEC_PUSH(scanner->context_stack, ctx);
            return accept_and_advance(lexer, TEMPLATE_INTERPOLATION_START);
        }
        // try to scan escape sequence
        if (lexer->lookahead == '$') {
            advance(lexer);
            if (lexer->lookahead == '{') {
                // $${
                advance(lexer);
                return finish_literal(scanner, lexer, valid_symbols);
            }
        }
        return finish_literal(scanner, lexer, valid_symbols);
    }
    if (valid_symbols[TEMPLATE_INTERPOLATION_END] && in_interpolation_context(scanner) && lexer->lookahead == '}') {
        VEC_POP(scanner->context_stack);
        return accept_and_advance(lexer, TEMPLATE_INTERPOLATION_END);
    }

    // manage template directives
    if (valid_symbols[TEMPLATE_DIRECTIVE_START] && valid_symbols[TEMPLATE_LITERAL_CHUNK] &&
        !in_directive_context(scanner) && lexer->lookahead == '%') {
        advance(lexer);
        if (lexer->lookahead == '{') {
            Context ctx = {0};
            ctx.type = TEMPLATE_DIRECTIVE;
            ctx.heredoc_identifier = string_new();
            VEC_PUSH(scanner->context_stack, ctx);
            return accept_and_advance(lexer, TEMPLATE_DIRECTIVE_START);
        }
        // try to scan escape sequence
        if (lexer->lookahead == '%') {
            advance(lexer);
            if (lexer->lookahead == '{') {
                // %%{
                advance(lexer);
                return finish_literal(scanner, lexer, valid_symbols);
            }
        }
        return finish_literal(scanner, lexer, valid_symbols);
    }
    if (valid_symbols[TEMPLATE_DIRECTIVE_END] && in_directive_context(scanner) && lexer->lookahead == '}') {
        VEC_POP(scanner->context_stack);
        return accept_and_advance(lexer, TEMPLATE_DIRECTIVE_END);
    }

    // manage heredoc context
    if (valid_symbols[HEREDOC_IDENTIFIER] && !in_heredoc_context(scanner)) {
        String identifier = string_new();
        if (!identifier_start(lexer->lookahead)) {
            STRING_FREE(identifier);
            return false;
        }
        do {
            char bytes[4];
            unsigned length = encode_codepoint(lexer->lookahead, bytes);
            // Preserve the existing opener bound independently of compact state.
            if (sizeof(uint32_t) * 2 + sizeof(ContextType) + identifier.len + length >= TREE_SITTER_SERIALIZATION_BUFFER_SIZE) {
                STRING_FREE(identifier);
                return false;
            }
            for (unsigned i = 0; i < length; i++) {
                STRING_PUSH(identifier, bytes[i]);
            }
            advance(lexer);
        } while (identifier_continue(lexer->lookahead));
        Context ctx = {0};
        ctx.type = HEREDOC_TEMPLATE;
        ctx.heredoc_identifier_length = identifier.len;
        if (identifier.len > HCL_HEREDOC_HASH_SIZE) {
            uint8_t digest[HCL_HEREDOC_HASH_SIZE];
            hcl_heredoc_hash(digest, identifier.data, identifier.len);
            memcpy(identifier.data, digest, sizeof(digest));
            identifier.len = sizeof(digest);
        }
        ctx.heredoc_identifier = identifier;
        VEC_PUSH(scanner->context_stack, ctx);
        return accept_inplace(lexer, HEREDOC_IDENTIFIER);
    }
    if (valid_symbols[HEREDOC_IDENTIFIER] && in_heredoc_context(scanner) && has_leading_whitespace_with_newline) {
        bool consumed;
        if (match_heredoc_prefix(&VEC_BACK(scanner->context_stack), lexer, &consumed)) {
            lexer->mark_end(lexer);
            while (iswspace(lexer->lookahead) && lexer->lookahead != '\n') advance(lexer);
            if (lexer->lookahead == '\n') {
                VEC_POP(scanner->context_stack);
                return accept_inplace(lexer, HEREDOC_IDENTIFIER);
            }
            // A failed own-line check must leave ${ and %{ intact. Keep the
            // accepted end before any trailing whitespace already inspected.
            return scan_literal_tail(scanner, lexer, valid_symbols);
        }
        if (consumed) return finish_literal(scanner, lexer, valid_symbols);
    }
    if (valid_symbols[TEMPLATE_LITERAL_CHUNK] && in_template_context(scanner)) {
        if (in_quoted_context(scanner) && lexer->lookahead == '\\') {
            if (!scan_quoted_escape(lexer)) return false;
        } else {
            advance(lexer);
        }
        return finish_literal(scanner, lexer, valid_symbols);
    }

    // probably not handled by the external scanner
    return false;
}

void *tree_sitter_hcl_external_scanner_create() {
    Scanner *scanner = calloc(1, sizeof(Scanner));
    scanner->context_stack.data = calloc(1, sizeof(Context));
    return scanner;
}

unsigned tree_sitter_hcl_external_scanner_serialize(void *payload, char *buffer) {
    Scanner *scanner = (Scanner *)payload;
    return serialize(scanner, buffer);
}

void tree_sitter_hcl_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    Scanner *scanner = (Scanner *)payload;
    deserialize(scanner, buffer, length);
}

bool tree_sitter_hcl_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    Scanner *scanner = (Scanner *)payload;
    return scan(scanner, lexer, valid_symbols);
}

void tree_sitter_hcl_external_scanner_destroy(void *payload) {
    Scanner *scanner = (Scanner *)payload;
    for (uint32_t i = 0; i < scanner->context_stack.len; i++) {
        STRING_FREE(scanner->context_stack.data[i].heredoc_identifier);
    }
    VEC_FREE(scanner->context_stack);
    free(scanner);
}
