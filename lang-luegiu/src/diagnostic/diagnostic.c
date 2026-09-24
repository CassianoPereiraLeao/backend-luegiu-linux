#include "../../include/diagnostic.h"

#define DIAG_MAX_CONTEXTS 40

#define DIAG_ERROR(Severity, ALIAS) \
    void diag_##ALIAS(DiagContext *ctx, const char* filename, size_t line, size_t col, const char* fmt, ...) {   \
        va_list args; \
        va_start(args, fmt); \
        diag_emit_v(ctx, Severity, filename, line, col, NULL, fmt, args); \
        va_end(args); \
    }

typedef struct {
    DiagContext* ctx;
    const char* filename;
    LineMap* map;
} ContextLineMap;

typedef struct LineIndex {
    const char* src;
    size_t* offsets;
    size_t count;
    struct LineIndex* next;
} LineIndex;

static LineIndex* g_line_indexes = NULL;
static ContextSource g_context_source[DIAG_MAX_CONTEXTS];
static size_t g_context_source_count = 0;
static ContextLineMap g_context_linemap[DIAG_MAX_CONTEXTS];
static size_t g_context_linemap_count = 0;

static ContextSource* find_or_create_slot(DiagContext *ctx) {
    for(size_t i = 0; i < g_context_source_count; ++i) 
        if(g_context_source[i].context == ctx)
            return &g_context_source[i];

    if(g_context_source_count >= DIAG_MAX_CONTEXTS) {
        fprintf(stderr, "diagnostico: limite de %d contextos atingidos, fontes nao serao rastreadas em mais contextos\n", DIAG_MAX_CONTEXTS);
        return NULL;
    }

    ContextSource* slot = &g_context_source[g_context_source_count++];
    slot->context = ctx;
    slot->source = NULL;
    return slot;
}

static const char* find_source(DiagContext *ctx, const char* filename) {
    if(!filename) return NULL;

    for(size_t i = 0; i < g_context_source_count; ++i) {
        if(g_context_source[i].context != ctx) continue;

        for(SourceFile* file = g_context_source[i].source; file != NULL; file = file->next)
            if(strcmp(file->filename, filename) == 0)
                return file->fullsource;
    }

    return NULL;
}

void diag_register_source(DiagContext *ctx, const char* filename, const char* src) {
    if(!filename) return;

    ContextSource* slot = find_or_create_slot(ctx);
    if(!slot) return;

    for(SourceFile* file = slot->source; file != NULL; file = file->next) 
        if(strcmp(file->filename, filename) == 0) return;

    size_t len = strlen(filename);
    char* filename_copy = (char*)arena_alloc(ctx->arena, len + 1);
    memcpy(filename_copy, filename, len + 1);

    size_t src_len = strlen(src);
    char* src_copy = (char*)arena_alloc(ctx->arena, src_len + 1);
    memcpy(src_copy, src, src_len + 1);

    SourceFile* file = (SourceFile*)arena_alloc(ctx->arena, sizeof(SourceFile));
    file->filename = filename_copy;
    file->fullsource = src_copy;
    file->next = slot->source;
    slot->source = file;
}

void diag_unregister(DiagContext *ctx) {
    for(size_t i = 0; i < g_context_source_count; ++i) {
        if(g_context_source[i].context == ctx) {
            g_context_source[i] = g_context_source[--g_context_source_count];
            break;
        }
    }
    for(size_t i = 0; i < g_context_linemap_count; ++i) {
        if(g_context_linemap[i].ctx == ctx) {
            g_context_linemap[i] = g_context_linemap[--g_context_linemap_count];
            break;
        }
    }
}

void diag_set_linemap(DiagContext *ctx, const char* filename, LineMap *map) {
    for(size_t i = 0; i < g_context_linemap_count; ++i) {
        if(g_context_linemap[i].ctx == ctx) {
            g_context_linemap[i].map = map;
            return;
        }
    }

    if(g_context_linemap_count >= DIAG_MAX_CONTEXTS) {
        fprintf(stderr, "diagnostics, limite de %d linemaps atingidos\n", DIAG_MAX_CONTEXTS);
        return;
    }

    g_context_linemap[g_context_linemap_count].ctx = ctx;
    g_context_linemap[g_context_linemap_count].filename = filename;
    g_context_linemap[g_context_linemap_count++].map = map;
}

static LineMap* find_linemap(DiagContext *ctx, const char* filename) {
    for(size_t i = 0; i < g_context_linemap_count; ++i)
        if(g_context_linemap[i].ctx == ctx &&
            g_context_linemap[i].filename && filename &&
            strcmp(g_context_linemap[i].filename, filename) == 0)
                return g_context_linemap[i].map;
    return NULL;
}

void diag_init(DiagContext *ctx, Arena *arena) {
    ctx->arena = arena;
    ctx->error_count = 0;
    ctx->max_errors = 0;
    ctx->warning_as_error = false;
    ctx->warning_count = 0;
    ctx->note_count = 0;
    ctx->first = NULL;
    ctx->last = NULL;
}

void diag_set_warning_as_error(DiagContext *ctx, bool value) {
    ctx->warning_as_error = value;
}

void diag_reset(DiagContext *ctx) {
    ctx->first = NULL;
    ctx->last = NULL;
    ctx->error_count = 0;
    ctx->note_count = 0;
    ctx->warning_count = 0;
}

static char* vformat(Arena *arena, const char* fmt, va_list args) {
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);

    if(needed < 0) needed = 0;

    char* buffer = (char*)arena_alloc(arena, (size_t)needed + 1);
    vsnprintf(buffer, (size_t)needed + 1, fmt, args);
    return buffer;
}

static LineIndex* build_line_index(Arena *arena, const char* src) {
    size_t line_count = 1;
    for(const char* peek = src; *peek; ++peek)
        if(*peek == '\n') line_count++;

    LineIndex* line_index = (LineIndex*)arena_alloc(arena, sizeof(LineIndex));
    line_index->src = src;
    line_index->count = line_count;
    line_index->offsets = (size_t*)arena_alloc(arena, line_count * sizeof(size_t));

    size_t index = 0;
    line_index->offsets[index++] = 0;

    for(const char* peek = src; *peek; ++peek) {
        if(*peek == '\n' && index < line_count) 
            line_index->offsets[index++] = (size_t)(peek - src) + 1;
    }

    line_index->next = g_line_indexes;
    g_line_indexes = line_index;
    return line_index;
}

static LineIndex* find_line_index(Arena *arena, const char* src) {
    if(!src) return NULL;
    for(LineIndex* line_index = g_line_indexes; line_index != NULL; line_index = line_index->next) {
        if(line_index->src == src) return line_index;
    }

    return build_line_index(arena, src);
}

static char* extract_line(Arena *arena, const char* src, size_t target) {
    if(!src || target == 0) return NULL;

    LineIndex* line = find_line_index(arena, src);
    if(!line || target > line->count) return NULL;

    const char* start = src + line->offsets[target - 1];
    const char* cursor = start;
    while(*cursor && *cursor != '\n') cursor++;

    size_t len = (size_t)(cursor - start);
    char* out = (char*)arena_alloc(arena, len + 1);
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

static void diag_emit_v(DiagContext *ctx, Severity severity, const char* filename, size_t line, size_t col, const char* code, const char* fmt, va_list args) {
    char* msg = vformat(ctx->arena, fmt, args);

    const char* resolved_file = filename;
    size_t resolved_line = line;

    LineMap* map = find_linemap(ctx, filename);
    if(map) {
        const char* mapped_file = NULL;
        int mapped_line = 0;
        linemap_resolve(map, (int)line, &mapped_file, &mapped_line);
        if(mapped_file) {
            resolved_file = mapped_file;
            resolved_line = (size_t)mapped_line;
        }
    }

    Diagnostic* diag = (Diagnostic*)arena_alloc(ctx->arena, sizeof(Diagnostic));
    diag->kind = INTERNAL;
    diag->severity = severity;
    diag->code = code;
    diag->msg = msg;
    diag->locale.col = col;
    diag->locale.line = resolved_line;
    diag->locale.filename = resolved_file;
    diag->next = NULL;

    const char* src = find_source(ctx, resolved_file);
    diag->src_line = extract_line(ctx->arena, src, resolved_line);

    if(!ctx->first) ctx->first = diag;
    else ctx->last->next = diag;

    ctx->last = diag;

    switch (severity)
    {
    case NOTE:
        ctx->note_count++;
        break;
    case WARNING:
        ctx->warning_count++;
        if(ctx->warning_as_error)
            ctx->error_count++;
        break;
    case ERROR:
    case FATAL:
        ctx->error_count++;
        break;
    }

    if(severity == FATAL) {
        diag_report_all(ctx);
        exit(COMPILE_ERROR);
    }

    if(ctx->max_errors > 0 && ctx->error_count >= ctx->max_errors) {
        diag_report_all(ctx);
        fprintf(stderr, "\n(parando: limite de erros %lld atingidos)\n", ctx->max_errors);
        exit(EXPLODE_MAX_ERRORS);
    }
}

void diag_emit(DiagContext *ctx, Severity severity, const char* filename, size_t line, size_t col, const char* code, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    diag_emit_v(ctx, severity, filename, line, col, code, fmt, args);
    va_end(args);
}

void diag_error_code(DiagContext *ctx, const char* filename, size_t line, size_t col, const char* code, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    diag_emit_v(ctx, ERROR, filename, line, col, code, fmt, args);
    va_end(args);
}

DIAG_ERROR(NOTE, note)
DIAG_ERROR(WARNING, warning)
DIAG_ERROR(ERROR, error)
DIAG_ERROR(FATAL, fatal)

#undef DIAG_ERROR

bool has_error(DiagContext *ctx) {
    return ctx->error_count > 0;
}

size_t diag_error_count(DiagContext *ctx) {
    return ctx->error_count;
}

size_t diag_warning_count(DiagContext *ctx) {
    return ctx->warning_count;
}

static const char* severity_label(Severity severity) {
    switch (severity)
    {
    case NOTE: return "nota";
    case WARNING: return "aviso";
    case ERROR: return "erro";
    case FATAL: return "fatal";
    default: return "?";
    }
}

static const char* severity_color(Severity severity) {
    switch (severity)
    {
    case NOTE: return "\x1b[36m";
    case WARNING: return "\x1b[33m";
    case ERROR:
    case FATAL:   return "\x1b[31m";
    default:      return "";
    }
}

static bool stdout_support_color(void) {
    const char* term = getenv("TERM");
    const char* no_color = getenv("NO_COLOR");

    if(no_color) return false;
    if(!term) return false;
    if(strcmp(term, "dump") == 0) return false;

    return true;
}

static void print_diagnostic(Diagnostic *diag) {
    const char* filename = diag->locale.filename ? diag->locale.filename : "<desconhecido>";
    bool use_color = stdout_support_color();
    const char* color = (use_color) ? severity_color(diag->severity) : "";
    const char* reset = (use_color) ? "\x1b[0m" : "";

    fprintf(stderr, "%s:%zu:%zu: %s%s%s", filename, diag->locale.line, diag->locale.col, color, severity_label(diag->severity), reset);
    if(diag->code) fprintf(stderr, " [%s]", diag->code);

    fprintf(stderr, ": %s\n", diag->msg);

    if(diag->src_line) {
        size_t src_len = strlen(diag->src_line);

        fprintf(stderr, "  %5zu | %s\n", diag->locale.line, diag->src_line);
        fprintf(stderr, "        | ");

        size_t col = diag->locale.col;

        for(size_t i = 1; i < col; ++i) {
            char c = (i - 1 < src_len) ? diag->src_line[i - 1] : ' ';
            putc(c == '\t' ? '\t' : ' ', stderr);
        }
        fprintf(stderr, "%s^%s\n", color, reset);
    }
}

size_t diag_report_all(DiagContext *ctx) {
    for(Diagnostic *diag = ctx->first; diag != NULL; diag = diag->next) {
        print_diagnostic(diag);
    }

    if(ctx->error_count > 0 || ctx->warning_count > 0) {
        fprintf(stderr, "\n");
        if (ctx->error_count > 0)
            fprintf(stderr, "%zu erro(s)", ctx->error_count);
        if (ctx->error_count > 0 && ctx->warning_count > 0)
            fprintf(stderr, ",");
        if (ctx->warning_count > 0)
            fprintf(stderr, "%zu aviso(s)", ctx->warning_count);
        fprintf(stderr, " gerados.\n");
    }

    return ctx->error_count;
}
