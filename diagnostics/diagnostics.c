#include "diagnostics.h"

static ContextSource g_context_sources[DIAG_MAX_CONTEXTS];
static size_t g_context_sources_count = 0;

static ContextSource* find_or_create_slot(DiagContext *context) {
    for(size_t i = 0; i < g_context_sources_count; ++i) 
        if(g_context_sources[i].context == context)
            return &g_context_sources[i];

    if(g_context_sources_count >= DIAG_MAX_CONTEXTS)
        return NULL;

    ContextSource* slot = &g_context_sources[g_context_sources_count++];
    slot->context = context;
    slot->source = NULL;
    return slot;
}

static const char* find_source(DiagContext *context, const char* filename) {
    if(!filename) return NULL;

    for(size_t i = 0; i < g_context_sources_count; ++i) {
        if(g_context_sources[i].context != context) continue;

        for(SourceFile* file = g_context_sources[i].source; file; file = file->next)
            if(strcmp(file->filename, filename) == 0)
                return file->fullsource;
    }

    return NULL;
}

void diag_register_source(DiagContext *context, const char* filename, const char* src) {
    if(!filename) return;

    ContextSource* slot = find_or_create_slot(context);
    if(!slot) return;

    for(SourceFile* file = slot->source; file; file = file->next)
        if(strcmp(file->filename, filename) == 0)
            return;

    size_t len = strlen(filename);
    char* filename_copy = (char*)arena_alloc(context->arena, len + 1);
    memcpy(filename_copy, filename, len + 1);

    SourceFile* file = (SourceFile*)arena_alloc(context->arena, sizeof(SourceFile));
    file->filename = filename_copy;
    file->fullsource = src;
    file->next = slot->source;
    slot->source = file;
}

void diag_init(DiagContext *context, Arena *arena) {
    context->arena = arena;
    context->error_count = 0;
    context->max_errors = 0;
    context->warning_as_errors = false;
    context->warning_count = 0;
    context->note_count = 0;
    context->first = NULL;
    context->last = NULL;
}

void diag_set_max_errors(DiagContext *context, size_t max_errors) {
    context->max_errors = max_errors;
}

void diag_set_warning_as_error(DiagContext *context, bool value) {
    context->warning_as_errors = value;
}

void diag_reset(DiagContext *context) {
    context->first = NULL;
    context->last = NULL;
    context->error_count = 0;
    context->warning_count = 0;
    context->note_count = 0;
}

static char* vformat(Arena *arena, const char* fmt, va_list args) {
    va_list args_copy;
    va_copy(args_copy, args);
    int needed = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);

    if(needed < 0) needed = 0;

    char* buffer = (char*)arena_alloc(arena, (size_t)needed + 1);
    vsnprintf(buffer, (size_t)needed + 1, fmt, args);
    return buffer;
}

static char* extract_line(Arena *arena, const char* src, size_t target) {
    if(!src) return NULL;

    const char* cursor = src;
    size_t current_line = 1;

    while(current_line < target && *cursor) {
        if(*cursor == '\n') current_line++;
        cursor++;
    }

    if(current_line != target) return NULL;

    const char* start = cursor;
    while(*cursor && *cursor != '\n') cursor++;

    size_t len = (size_t)(cursor - start);
    char* out = (char*)arena_alloc(arena, len + 1);
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

static void diag_emit_v(DiagContext *context, DiagSeverity severity, const char* file, size_t line, size_t col, const char* code, const char* fmt, va_list args) {
    char* message = vformat(context->arena, fmt, args);

    Diagnostic* diag = (Diagnostic*)arena_alloc(context->arena, sizeof(Diagnostic));
    diag->type = INTERNAL;
    diag->severity = severity;
    diag->code = code;
    diag->message = message;
    diag->locale.filename = file;
    diag->locale.line = line;
    diag->locale.col = col;
    diag->next = NULL;

    const char* src = find_source(context, file);
    diag->src_line = extract_line(context->arena, src, line);
    
    if(!context->first) context->first = diag;
    else context->last->next = diag;
    context->last = diag;

    switch (severity)
    {
    case NOTE:
        context->note_count++;
        break;
    case WARNING:
        context->warning_count++;
        if(context->warning_as_errors)
            context->error_count++;
        break;
    case ERROR:
    case FATAL:
        context->error_count++;
        break;
    }

    if(severity == FATAL) {
        diag_report_all(context);
        exit(5);
    }

    if(context->max_errors > 0 && context->error_count >= context->max_errors) {
        diag_report_all(context);
        fprintf(stderr, "\n(parando: limite de %zu erro(s) atingido)\n", context->max_errors);
        exit(8);
    }
}

void diag_emit(DiagContext *context, DiagSeverity severity, const char* file, size_t line, size_t col, const char* code, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    diag_emit_v(context, severity, file, line, col, code, fmt, args);
    va_end(args);
}

void diag_note(DiagContext *context, const char* file, size_t line, size_t col, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    diag_emit_v(context, NOTE, file, line, col, NULL, fmt, args);
    va_end(args);
}

void diag_warning(DiagContext *context, const char* file, size_t line, size_t col, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    diag_emit_v(context, WARNING, file, line, col, NULL, fmt, args);
    va_end(args);
}

void diag_error(DiagContext *context, const char* file, size_t line, size_t col, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    diag_emit_v(context, ERROR, file, line, col, NULL, fmt, args);
    va_end(args);
}

void diag_fatal(DiagContext *context, const char* file, size_t line, size_t col, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    diag_emit_v(context, FATAL, file, line, col, NULL, fmt, args);
    va_end(args);
}

void diag_error_code(DiagContext *context, const char* file, size_t line, size_t col, const char* code, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    diag_emit_v(context, ERROR, file, line, col, code, fmt, args);
    va_end(args);
}

bool has_error(DiagContext *context) {
    return context->error_count > 0;
}

size_t diag_error_count(DiagContext *context) {
    return context->error_count;
}

size_t diag_warning_count(DiagContext *context) {
    return context->warning_count;
}

static const char* severity_label(DiagSeverity severity) {
    switch (severity) {
        case NOTE:    return "nota";
        case WARNING: return "aviso";
        case ERROR:   return "erro";
        case FATAL:   return "erro fatal";
        default:      return "?";
    }
}

static const char* severity_color(DiagSeverity severity) {
    switch (severity) {
        case NOTE:    return "\x1b[36m";
        case WARNING: return "\x1b[33m";
        case ERROR:
        case FATAL:   return "\x1b[31m";
        default:      return "";
    }
}

static bool stdout_support_color(void) {
    const char *term = getenv("TERM");
    const char *no_color = getenv("NO_COLOR");

    if (no_color)
        return false;
    if (!term)
        return false;
    if (strcmp(term, "dump") == 0)
        return false;

    return true;
}

static void print_diagnostic(Diagnostic *diag) {
    const char* file = diag->locale.filename ? diag->locale.filename : "<desconhecido>";
    bool use_color = stdout_support_color();
    const char* color = use_color ? severity_color(diag->severity) : "";
    const char* reset = use_color ? "\x1b[0m" : "";

    fprintf(stderr, "%s:%zu:%zu: %s%s%s", file, diag->locale.line, diag->locale.col, color, severity_label(diag->severity), reset);

    if(diag->code) fprintf(stderr, " [%s]", diag->code);

    fprintf(stderr, ": %s\n", diag->message);

    if(diag->src_line) {
        size_t src_len = strlen(diag->src_line);

        fprintf(stderr, "  %5zu | %s\n", diag->locale.line, diag->src_line);
        fprintf(stderr, "        | ");
        size_t col = diag->locale.col;
        for(size_t i = 0; i < col; ++i) {
            char c = (i - 1 < src_len) ? diag->src_line[1] : ' ';
            putc(c == '\t' ? '\t' : ' ', stderr);
        }
        fprintf(stderr, "%s^%s\n", color, reset);
    }
}

size_t diag_report_all(DiagContext *context) {
    for(Diagnostic *diag = context->first; diag; diag = diag->next) 
        print_diagnostic(diag);

    if(context->error_count > 0 || context->warning_count > 0) {
        fprintf(stderr, "\n");
        if (context->error_count > 0)
            fprintf(stderr, "%zu erro(s)", context->error_count);
        if (context->error_count > 0 && context->warning_count > 0)
            fprintf(stderr, ",");
        if (context->warning_count > 0)
            fprintf(stderr, "%zu aviso(s)", context->warning_count);
        fprintf(stderr, " gerados.\n");
    }

    return context->error_count;
}
