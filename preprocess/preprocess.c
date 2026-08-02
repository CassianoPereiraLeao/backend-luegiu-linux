#if defined(__unix__) || defined(__APPLE__)
    #define _POSIX_C_SRC 200809L
    #define _DEFAULT_SRC 1
#endif

#define SB_NULL_TERMINATE (sb->data[sb->len] = '\0')
#define PATH_MAX 4096

#include "preprocess.h"
#include <limits.h>
#include <stdlib.h>

static void preprocess_internal(const char* source, const char* filename, const char* base_dir, const char* lib_dir,
    Arena *arena, int depth, LineMap *map, int *out_line, StringBuilder *sb);

static IncludeFile* g_included = NULL;
static Arena* g_included_arena = NULL;

static void string_builder_init(StringBuilder *sb, Arena *arena) {
    sb->data = (char*)arena_alloc(arena, STRING_BUILDER_INITIAL_CAP);
    sb->data[0] = '\0';
    sb->len = 0;
    sb->cap = STRING_BUILDER_INITIAL_CAP;
    sb->arena = arena;
}

static void string_builder_ensure(StringBuilder *sb, size_t addit) {
    if(sb->len + addit + 1 <= sb->cap) return;

    size_t new_cap = sb->cap;
    while(new_cap < sb->len + addit + 1) {
        if(new_cap > (SIZE_MAX / 2)) {
            fprintf(stderr, "Preprocessador: overflow de capacidade de buffer\n");
            exit(1);
        }
        new_cap *= 2;
    }

    if(new_cap > MAX_BUFFER_SIZE) {
        fprintf(stderr,
            "Preprocessador: saida excedeu o limite maximo de %zu MB.\n"
            "Isso normalmente indica um ciclo de includes ou uma macro que\n"
            "se expande de forma explosiva (macro bomb).\n",
            (size_t)(MAX_BUFFER_SIZE / MB));
        exit(1);
    }

    char* new_data = (char*)arena_alloc(sb->arena, new_cap);
    memcpy(new_data, sb->data, sb->len);
    sb->data = new_data;
    sb->cap = new_cap;
}

static void string_builder_write(StringBuilder *sb, const char* str, size_t len) {
    if(len == 0) return;
    string_builder_ensure(sb, len);
    memcpy(sb->data + sb->len, str, len);
    sb->len += len;
    SB_NULL_TERMINATE;
}

static void string_builder_putchar(StringBuilder *sb, char c) {
    string_builder_ensure(sb, 1);
    sb->data[sb->len++] = c;
    SB_NULL_TERMINATE;
}

static char* string_builder_finish(StringBuilder *sb) {
    SB_NULL_TERMINATE;
    return sb->data;
}

void linemap_init(LineMap *map) {
    map->first = NULL;
    map->last = NULL;
    map->count = 0;
}

static void linemap_push(LineMap *map, Arena *arena, int out_line, int src_line, const char* file) {
    if(!map) return;

    if(map->last && map->last->out_line == out_line) {
        map->last->src_line = src_line;
        map->last->file = file;
        return;
    }

    LineMapEntry* entry = (LineMapEntry*)arena_alloc(arena, sizeof(LineMapEntry));
    entry->out_line = out_line;
    entry->src_line = src_line;
    entry->file = file;
    entry->next = NULL;

    if(map->last) map->last->next = entry;
    else map->first = entry;

    map->last = entry;
    map->count++;
}

void linemap_resolve(LineMap *map, int out_line, const char** file_out, int *line_out) {
    if(!map || !map->first) {
        *file_out = "?";
        *line_out = out_line;
        return;
    }

    LineMapEntry* first = map->first;
    for(LineMapEntry* entry = map->first; entry && entry->out_line <= out_line; entry = entry->next) {
        first = entry;
    }

    *file_out = first->file;
    *line_out = first->src_line + (out_line - first->out_line);
}

static void included_init(Arena *arena) {
    g_included = NULL;
    g_included_arena = arena;
}

static bool included_check(const char* path) {
    for(IncludeFile* file = g_included; file; file = file->next) {
        if(strcmp(file->path, path) == 0) return true;
    }

    return false;
}

static void included_add(const char* path) {
    IncludeFile* file = (IncludeFile*)arena_alloc(g_included_arena, sizeof(IncludeFile));
    size_t len = strlen(path);
    char* copy = (char*)arena_alloc(g_included_arena, len + 1);
    memcpy(copy, path, len + 1);
    file->path = path;
    file->next = g_included;
    g_included= file;
}

static char* get_dir(const char* filepath, Arena *arena) {
    const char* last_separation = NULL;

    for(const char* peek = filepath; *peek; ++peek) {
        if(*peek == '/' || *peek == '\\') last_separation = peek;
    }

    if(!last_separation) {
        char* dot = (char*)arena_alloc(arena, 2);
        dot[0] = '.';
        dot[1] = '\0';
        return dot;
    }

    size_t len = last_separation - filepath;
    char* dir = (char*)arena_alloc(arena, len + 1);
    memcpy(dir, filepath, len);
    dir[len] = '\0';
    return dir;
}

static bool is_identifier(char c) {
    return (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '_';
}

static char* join_path(const char* dir, const char* file, Arena *arena) {
    size_t dir_len = strlen(dir);
    size_t file_len = strlen(file);
    char* path = (char*)arena_alloc(arena, dir_len + file_len + 2);
    memcpy(path, dir, dir_len);
    if(dir_len > 0 && dir[dir_len - 1] != '/' && dir[dir_len - 1] != '\\')
        path[dir_len++] = '/';
    memcpy(path + dir_len, file, file_len);
    path[dir_len + file_len] = '\0';
    return path;
}

static bool path_has_traversal(const char* filename) {
    if(filename[0] == '/' || filename[0] == '\\') return true;
    if(filename[0] != '\0' && filename[1] == ':') return true;

    const char* peek = filename;
    while(*peek) {
        if(peek[0] == '.' && peek[1] == '.' &&
            (peek[2] == '/' || peek[2] == '\\' || peek[2] == '\0') &&
            (peek == filename || peek[-1] == '/' || peek[-1] == '\\'))
            return true;
        peek++;
    }

    return false;
}

static char* read_file(const char* path, Arena *arena) {
    FILE* f = fopen(path, "rb");
    if(!f) return NULL;

    if(fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }

    long len = ftell(f);

    if((size_t)len > MAX_FILE_SIZE) {
        fprintf(stderr,
            "Preprocessador: arquivo '%s' excede o tamanho maximo permitido "
            "(%zu MB)\n", path, (size_t)(MAX_FILE_SIZE / MB));
        fclose(f);
        exit(1);
    }

    if(fseek(f, 0, SEEK_SET) != 0) { 
        fclose(f); 
        return NULL;
    }

    char* buffer = (char*)arena_alloc(arena, (size_t)len + 1);
    size_t bytes = fread(buffer, 1, (size_t)len, f);
    buffer[bytes] = '\0';
    fclose(f);
    return buffer;
}

static char* resolve_and_check_path(const char* root_dir, const char* filename, Arena *arena) {
    if(path_has_traversal(filename)) {
        fprintf(stderr,
            "Preprocessador: path invalido em 'extract': '%s'. "
            "Paths absolutos ou com '..' nao sao permitidos.\n", filename);
        exit(1);
    }

    return join_path(root_dir, filename, arena);
}

static char* dedup(const char* resolved_path, Arena *arena) {
#if defined(__unix__) || defined(__APPLE__)
    char real[PATH_MAX];
    if (realpath(resolved_path, real) != NULL) {
        size_t len = strlen(real);
        char* copy = (char*)arena_alloc(arena, len + 1);
        memcpy(copy, real, len + 1);
        return copy;
    }
#endif
    size_t len = strlen(resolved_path);
    char* copy = (char*)arena_alloc(arena, len + 1);
    memcpy(copy, resolved_path, len + 1);
    return copy;
}

static IncludePolicy parse_use_directive(const char* source, const char** out) {
    IncludePolicy policy = INCLUDE_MULTIPLE;
    const char* peek = source;

    while(*peek == ' ' || *peek == '\t') peek++;

    if(strncmp(peek, "target", 6) != 0 || is_identifier(peek[6])) {
        *out = source;
        return INCLUDE_MULTIPLE;
    }

    peek += 6;

    while(*peek == ' ' || *peek == '\t') peek++;
    if(*peek != '-' || *(peek + 1) != '>') {
        *out = source;
        return INCLUDE_MULTIPLE;
    }

    peek += 2;

    while(*peek == ' ' || *peek == '\t') peek++;
    if(strncmp(peek, "single", 6) == 0 && !is_identifier(peek[6])) {
        policy = INCLUDE_SINGLE;
        peek += 6;
    } else if(strncmp(peek, "multiple", 8) == 0 && !is_identifier(peek[8])) {
        policy = INCLUDE_MULTIPLE;
        peek += 8;
    } else {
        fprintf(stderr, "Preprocessador: argumento invalido em 'use target'. "
            "Use 'single' ou 'multiple'\n");
        exit(1);
    }

    while(*peek && *peek != '\n') peek++;
    if(*peek == '\n') peek++;

    *out = peek;
    return policy;
}

static char* parse_extract_target(const char** src_ptr, Arena *arena, char* out_type) {
    const char* source = *src_ptr;
    char close;

    if(*source == '"') {
        close = '"';
        *out_type = 'p';
    } else if(*source == '[') {
        close = ']';
        *out_type = 'l';
    } else {
        fprintf(stderr, "Preprocessador: sintaxe invalida em 'extract'. "
            "Use '\"arquivo.lh\"' ou '[stdlib]'\n");
        exit(1);
    }

    source++;
    const char* start = source;
    while(*source && *source != close && *source != '\n') source++;

    if(*source != close) {
        fprintf(stderr, "Preprocessador: delimitador '%c' nao fechado em 'extract'\n", close);
        exit(1);
    }

    size_t len = source - start;
    source++;

    char* target = (char*)arena_alloc(arena, len + 1);
    memcpy(target, start, len);
    target[len] = '\0';

    *src_ptr = source;
    return target;
}

static bool file_declares_single(const char* file_src) {
    const char* peek = file_src;

    for(;;) {
        const char* line_start = peek;
        while(*peek == ' ' || *peek == '\t') peek++;

        if(*peek == '\n') { 
            peek++;
            continue;
        }
        
        if(*peek == '\0') return false;

        if(strncmp(peek, "use", 3) == 0 && !is_identifier(peek[3])) {
            const char* after_keyword = peek + 3;
            while(*after_keyword == ' ' || *after_keyword == '\t') after_keyword++;

            const char* after = { 0 };
            IncludePolicy policy = parse_use_directive(after_keyword, &after);
            if(after != after_keyword) return policy == INCLUDE_SINGLE;
        }

        (void)line_start;
        return false;
    }
}

static void expand_extracts(const char* src, const char* filename, const char* base_dir, const char* lib_dir,
    Arena *arena, int depth, LineMap *map, int *out_line, StringBuilder *sb) {

    linemap_push(map, arena, *out_line + 1, 1, filename);

    const char* source = src;
    int local_line = 1;

    while(*source) {
        const char* iter_start = source;
        while(*source == ' '|| *source == '\t') source++;

        bool handled = false;

        if(strncmp(source, "use", 3) == 0 && !is_identifier(source[3])) {
            const char* after_keyword = source + 3;
            while(*after_keyword == ' ' || *after_keyword == '\t') after_keyword++;

            const char* after = after_keyword;
            IncludePolicy parsed_policy = parse_use_directive(after_keyword, &after);
            (void)parsed_policy;

            if(after != after_keyword) {
                source = after;
                string_builder_putchar(sb, '\n');
                (*out_line)++;
                handled = true;
            }
        }

        if(!handled && strncmp(source, "extract", 7) == 0 && !is_identifier(source[7])) {
            const char* after_keyword = source + 7;
            while(*after_keyword == ' ' || *after_keyword == '\t') after_keyword++;

            const char* peek = after_keyword;
            if(*peek == '-' && *(peek + 1) == '>') peek += 2;
            while(*peek == ' ' || *peek == '\t') peek++;

            char type = 0;
            char* target_name = parse_extract_target(&peek, arena, &type);
            source = peek;

            const char* root_dir = (type == 'l') ? lib_dir : base_dir;
            char* resolved_path = resolve_and_check_path(root_dir, target_name, arena);
            char* file_src = read_file(resolved_path, arena);

            if(!file_src) {
                fprintf(stderr, "Preprocessador: arquivo nao encontrado: '%s' "
                                "(includado a partir de '%s', linha %d)\n",
                                resolved_path, filename, local_line);
                exit(1);
            }

            char* new_base_dir = get_dir(resolved_path, arena);
            char* canonical = dedup(resolved_path, arena);

            while(*source && *source != '\n') source++;
            if(*source == '\n') source++;

            bool single_mode = file_declares_single(file_src);
            if(single_mode && included_check(canonical)) {
                string_builder_putchar(sb, '\n');
                (*out_line)++;
                handled = true;
            } else {
                if(single_mode) included_add(canonical);

                int line_before = local_line;

                preprocess_internal(file_src, resolved_path, new_base_dir, lib_dir, arena, depth + 1, map, out_line, sb);

                if(sb->len == 0 || sb->data[sb->len - 1] != '\n') {
                    string_builder_putchar(sb, '\n');
                    (*out_line)++;
                }

                linemap_push(map, arena, *out_line + 1, line_before + 1, filename);
                handled = true;
            }
        }

        if(!handled) {
            source = iter_start;
            while(*source && *source != '\n') {
                string_builder_putchar(sb, *source);
                source++;
            }

            if(*source == '\n') {
                string_builder_putchar(sb, '\n');
                source++;
                (*out_line)++;
            }
        }

        for(const char* peek = iter_start; peek < source; peek++)
            if(*peek == '\n') local_line++;
    }
}

static void preprocess_internal(const char* source, const char* filename, const char* base_dir, const char* lib_dir,
    Arena *arena, int depth, LineMap *map, int *out_line, StringBuilder *sb) {

    if(depth > MAX_INCLUDE_DEPTH) {
        fprintf(stderr,
            "Preprocessador: profundidade maxima de includes (%d) excedida "
            "ao processar '%s'.\nIsso normalmente indica um ciclo de "
            "includes (ex: A inclui B que inclui A de volta).\n",
            MAX_INCLUDE_DEPTH, filename);
        exit(1);
    }

    expand_extracts(source, filename, base_dir, lib_dir, arena, depth, map, out_line, sb);
}

static Macro* collect_macros(const char* src, char* out, Arena *arena) {
    Macro* first = NULL;
    const char* source = src;
    char* w = out;

    while(*source) {
        const char* line_start = source;
        while(*source == ' ' || *source == '\t') source++;

        const char* keyword = "preprocess";
        size_t keyword_len = 10;
        bool is_preprocess = (strncmp(source, keyword, keyword_len) == 0 && !is_identifier(source[keyword_len]));

        if(is_preprocess) {
            source += keyword_len;
            while(*source == ' ' || *source == '\t') source++;

            const char* start = source;
            while(is_identifier(*source)) source++;
            size_t len = source - start;

            if(len == 0) {
                fprintf(stderr, "Preprocessador: 'preprocess' sem nome de macro valido\n");
                exit(1);
            }

            char* name_copy = (char*)arena_alloc(arena, len + 1);
            memcpy(name_copy, start, len);
            name_copy[len] = '\0';

            while(*source == ' ' || *source == '\t') source++;
            if(*source == '-' && *(source + 1) == '>') source += 2;
            while(*source == ' ' || *source == '\t') source++;

            const char* value_start = source;
            while(*source && *source != '\n') source++;
            size_t value_len = source - value_start;

            while(value_len > 0 && value_start[value_len - 1] == ' ') value_len--;
            while(value_len > 0 && value_start[value_len - 1] == '\r') value_len--;

            char* value_copy = (char*)arena_alloc(arena, value_len + 1);
            memcpy(value_copy, value_start, value_len);
            value_copy[value_len] = '\0';

            Macro* macro = (Macro*)arena_alloc(arena, sizeof(Macro));
            macro->name = name_copy;
            macro->name_len = len;
            macro->value = value_copy;
            macro->value_len = value_len;
            macro->next = first;
            first = macro;

            *w++ = '\n';
            if(*source == '\n') source++;
        } else {
            source = line_start;
            while(*source && *source != '\n') *w++ = *source++;
            if(*source == '\n') *w++ = *source++;
        }
    }

    *w = '\0';
    return first;
}

static char* expand_macros(const char* src, Macro *macros, Arena *arena) {
    StringBuilder sb;
    string_builder_init(&sb, arena);

    const char* source = src;

    while(*source) {
        if(*source == '"') {
            const char* start = source;
            source++;

            while(*source && *source != '"') {
                if(*source == '\\' && *(source + 1)) source++;
                source++;
            }

            if(*source) source++;
            string_builder_write(&sb, start, (size_t)(source - start));
            continue;
        }

        if(*source == '\'') {
            const char* start = source;
            source++;

            while(*source && *source != '\'') {
                if(*source == '\\' && *(source + 1)) source++;
                source++;
            }

            if(*source) source++;
            string_builder_write(&sb, start, (size_t)(source - start));
            continue;
        }

        if(*source == '/' && *(source + 1) == '/') {
            const char* start = source;
            while(*source && *source != '\n') source++;
            string_builder_write(&sb, start, (size_t)(source - start));
            continue;
        }

        if(strncmp(source, "/comment", 8) == 0 && !is_identifier(source[8])) {
            source += 8;
            const char* end_mark = "endcomment/";
            size_t end_mark_len = 11;
            int newline_comment = 0;

            while(*source && strncmp(source, end_mark, end_mark_len) != 0) {
                if(*source == '\n') newline_comment++;
                source++;
            }

            if(*source) source += end_mark_len;
            else {
                fprintf(stderr, "Preprocessador: comentario de bloco nao terminado "
                                "(esperado 'endcomment/')\n");
                exit(1);
            }

            for(int i = 0; i < newline_comment; ++i) string_builder_putchar(&sb, '\n');
            continue;
        }

        if(is_identifier(*source) && !(*source >= '0' && *source <= '9')) {
            const char* start = source;
            while(is_identifier(*source)) source++;
            size_t len = source - start;

            Macro* found = NULL;
            for(Macro* macro = macros; macro; macro = macro->next) {
                if(macro->name_len == len && strncmp(macro->name, start, len) == 0) {
                    found = macro;
                    break;
                }
            }

            if(found) string_builder_write(&sb, found->value, found->value_len);
            else string_builder_write(&sb, start, len);
            continue;
        }

        string_builder_putchar(&sb, *source);
        source++;
    }

    return string_builder_finish(&sb);
}

static char* expand_macros_recursive(const char* src, Macro *macros, Arena *arena) {
    char* result = (char*)src;
    for(int pass = 0; pass < 10; ++pass) {
        char* next = expand_macros(result, macros, arena);
        if(strcmp(next, result) == 0) break;
        result = next;
    }
    return result;
}

char* preprocess_source(const char* source, const char* base_dir, const char* lib_dir, const char* filename,
    Arena *arena, LineMap *out) {

    included_init(arena);

    LineMap local_map;
    LineMap* map = out ? out : &local_map;

    linemap_init(map);
    
    StringBuilder sb;
    string_builder_init(&sb, arena);

    int out_line = 0;
    preprocess_internal(source, filename, base_dir, lib_dir, arena, 0, map, &out_line, &sb);

    char* full_extract = string_builder_finish(&sb);

    size_t len = strlen(full_extract);
    char* stripped = (char*)arena_alloc(arena, len + 1);
    Macro* macros = collect_macros(full_extract, stripped, arena);

    if(!macros) return stripped;
    return expand_macros_recursive(stripped, macros, arena);
}
