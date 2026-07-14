#pragma once

#include "../common.h"
#include "../arena/arena.h"
#include <stdarg.h>

#define DIAG_MAX_CONTEXTS 8

typedef enum { 
    LEXICAL,
    SYNTAX,
    SEMANTICAL,
    PREPROCESS,
    INTERNAL
} DiagType;

typedef enum {
    NOTE,
    WARNING,
    ERROR,
    FATAL
} DiagSeverity;

typedef struct {
    const char* filename;
    size_t line;
    size_t col;
} DiagLocale;

typedef struct Diagnostic {
    DiagType type;
    DiagSeverity severity;
    DiagLocale locale;
    char* message;
    char* src_line;
    const char* code;
    struct Diagnostic* next;
} Diagnostic;

struct DiagContext {
    Arena* arena;
    Diagnostic* first;
    Diagnostic* last;

    size_t error_count;
    size_t warning_count;
    size_t note_count;

    size_t max_errors;
    bool warning_as_errors;
};

typedef struct SourceFile {
    const char* filename;
    const char* fullsource;
    struct SourceFile* next;
} SourceFile;

typedef struct {
    DiagContext* context;
    SourceFile* source;
} ContextSource;

void diag_init(DiagContext *context, Arena *arena);
void diag_set_max_errors(DiagContext *context, size_t max_errors);
void diag_set_warning_as_error(DiagContext *context, bool value);
void diag_register_source(DiagContext *context, const char* filename, const char* src);
void diag_emit(DiagContext *context, DiagSeverity severity, const char* file, size_t line, size_t col, const char* code, const char* fmt, ...);
void diag_note(DiagContext *context, const char* file, size_t line, size_t col, const char* fmt, ...);
void diag_warning(DiagContext *context, const char* file, size_t line, size_t col, const char* fmt, ...);
void diag_error(DiagContext *context, const char* file, size_t line, size_t col, const char* fmt, ...);
void diag_fatal(DiagContext *context, const char* file, size_t line, size_t col, const char* fmt, ...);
void diag_error_code(DiagContext *context, const char* file, size_t line, size_t col, const char* code, const char* fmt, ...);
bool has_error(DiagContext *context);
size_t diag_error_count(DiagContext *context);
size_t diag_warning_count(DiagContext *context);
size_t diag_report_all(DiagContext *context);
void diag_reset(DiagContext *context);
