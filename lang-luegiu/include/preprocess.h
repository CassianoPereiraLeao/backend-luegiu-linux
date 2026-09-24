#pragma once

#include "common.h"
#include "arena.h"
#include "../src/helpers/exitcodes.h"
#include "string_view_helper.h"

#define MB (1024U * 1024U)

#define MAX_INCLUDE_DEPTH 200
#define MAX_FILE_SIZE (64U * MB)
#define MAX_BUFFER_SIZE (512U * MB)
#define STRING_BUILDER_INIT_CAP (4U * 1024U)

typedef void (*SourceRegisterFn)(void *userdata, const char* filename, const char* src);

typedef enum {
    INCLUDE_SINGLE,
    INCLUDE_MULTIPLE
} IncludePolicy;

typedef struct IncludeFile {
    const char* path;
    struct IncludeFile* next;
} IncludeFile;

typedef struct Macro {
    const char* name;
    size_t name_len;
    const char* value;
    size_t value_len;
    struct Macro* next;
} Macro;

typedef struct LineMapEntry {
    int out_line;
    int src_line;
    const char *file;
    struct LineMapEntry* next;
} LineMapEntry;

typedef struct LineMap {
    LineMapEntry* first;
    LineMapEntry* last;
    int count;
} LineMap;

typedef struct {
    char* data;
    size_t len;
    size_t capacity;
    Arena* arena;
} StringBuilder;

void linemap_init(LineMap *map);
void linemap_resolve(LineMap *map, int out_line, const char** file_out, int *line_out);
char* preprocess_source(const char* src, const char* base_dir, const char* lib_dir, const char* file, Arena *arena, LineMap *out,
    SourceRegisterFn register_fn, void *userdata);
