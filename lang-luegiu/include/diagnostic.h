#pragma once

#include "common.h"
#include "arena.h"
#include "preprocess.h"
#include "../src/helpers/exitcodes.h"

typedef enum {
    PREPROCESS,
    LEXICAL,
    SYNTAX,
    SEMANTICAL,
    INTERNAL
} DiagType;

typedef enum {
    NOTE,
    WARNING,
    ERROR,
    FATAL
} Severity;

typedef struct {
    const char* filename;
    size_t line;
    size_t col;
} DiagLocale;

typedef struct Diagnostic {
    DiagType kind;
    Severity severity;
    DiagLocale locale;
    char* msg;
    char* src_line;
    const char* code;
    struct Diagnostic* next;
} Diagnostic;

typedef struct {
    Arena* arena;
    Diagnostic* first;
    Diagnostic* last;

    size_t error_count;
    size_t warning_count;
    size_t note_count;

    size_t max_errors;
    bool warning_as_error;
} DiagContext;

typedef struct SourceFile {
    const char* filename;
    const char* fullsource;
    struct SourceFile* next;
} SourceFile;

typedef struct {
    DiagContext* context;
    SourceFile* source;
} ContextSource;

void diag_init(DiagContext *ctx, Arena *arena);
void diag_set_warning_as_error(DiagContext *ctx, bool value);
void diag_register_source(DiagContext *ctx, const char* filename, const char* src);
void diag_emit(DiagContext *ctx, Severity severity, const char* filename, size_t line, size_t col, const char* code, const char* fmt, ...);
void diag_note(DiagContext *ctx, const char* filename, size_t line, size_t col, const char* fmt, ...);
void diag_warning(DiagContext *ctx, const char* filename, size_t line, size_t col, const char* fmt, ...);
void diag_error(DiagContext *ctx, const char* filename, size_t line, size_t col, const char* fmt, ...);
void diag_fatal(DiagContext *ctx, const char* filename, size_t line, size_t col, const char* fmt, ...);
void diag_set_linemap(DiagContext *ctx, const char* filename, LineMap *map);
void diag_error_code(DiagContext *ctx, const char* filename, size_t line, size_t col, const char* code, const char* fmt, ...);
bool has_error(DiagContext *context);
size_t diag_error_count(DiagContext *ctx);
size_t diag_warning_count(DiagContext *ctx);
size_t diag_report_all(DiagContext *ctx);
void diag_reset(DiagContext *ctx);
void diag_unregister(DiagContext *ctx);
