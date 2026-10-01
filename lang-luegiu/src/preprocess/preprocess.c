#include "../../include/preprocess.h"

#if defined(__unix__) || defined(__APPLE__)
    #define _POSIX_C_SRC 200809L
    #define _DEFAULT_SRC 1
#endif

#define SB_NULL_TERMINATE (sb->data[sb->len] = '\0')

static void preprocess_internal(const char* source, const char* filename, const char* base_dir, const char* lib_dir,
    Arena *arena, int depth, LineMap *map, int *out_line, StringBuilder *sb,
    SourceRegisterFn register_fn, void* userdata);

static IncludeFile* g_included = NULL;
static Arena* g_included_arena = NULL;

static void string_builder_init(StringBuilder *sb, Arena *arena) {
    sb->data = (char*)arena_alloc(arena, STRING_BUILDER_INIT_CAP);
    sb->data[0] = '\0';
    sb->len = 0;
    sb->capacity = STRING_BUILDER_INIT_CAP;
    sb->arena = arena;
}

static void string_builder_ensure(StringBuilder *sb, size_t addit) {
    if(sb->len + addit + 1 <= sb->capacity) return;

    size_t new_cap = sb->capacity;
    while(new_cap < sb->len + addit + 1) {
        if(new_cap > (SIZE_MAX / 2)) {
            fprintf(stderr, "Preprocessador: overflow de capacidade do buffer\n");
            exit(PREPROCESS_ERROR);
        }
        new_cap *= 2;
    }

    if(new_cap > MAX_BUFFER_SIZE) {
        fprintf(stderr, 
            "Preprocessador: saida excedeu o limite maximo de %zu MB.\n"
            "Isso normalmente indica um ciclo de includes ou uma macro que\n"
            "se expande de forma explosiva (macro bomb).\n",
            (size_t)(MAX_BUFFER_SIZE / MB));
        exit(PREPROCESS_ERROR);
    }

    char* new_data = (char*)arena_alloc(sb->arena, new_cap);
    memcpy(new_data, sb->data, sb->len);
    sb->data = new_data;
    sb->capacity = new_cap;
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

static void linemap_push(LineMap *map, Arena *arena, int out_line, int src_line, const char* filename) {
    if(!map) return;

    if(map->last && map->last->out_line == out_line) {
        map->last->src_line = src_line;
        map->last->file = filename;
        return;
    }

    LineMapEntry* entry = (LineMapEntry*)arena_alloc(arena, sizeof(LineMapEntry));
    entry->out_line = out_line;
    entry->src_line = src_line;
    entry->file = filename;
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

    LineMapEntry* best = map->first;
    for(LineMapEntry* entry = map->first; entry != NULL; entry = entry->next) {
        if(entry->out_line > out_line) break;
        best = entry;
    }

    *file_out = best->file;
    *line_out = best->src_line + (out_line - best->out_line);
}

static void included_init(Arena *arena) {
    g_included = NULL;
    g_included_arena = arena;
}

static bool included_check(const char* path) {
    for(IncludeFile* file = g_included; file != NULL; file = file->next)
        if(strcmp(file->path, path) == 0) return true;

    return false;
}

static void included_add(const char* path) {
    IncludeFile* file = (IncludeFile*)arena_alloc(g_included_arena, sizeof(IncludeFile));
    size_t len = strlen(path);
    char* copy = (char*)arena_alloc(g_included_arena, len + 1);
    memcpy(copy, path, len + 1);
    file->path = path;
    file->next = g_included;
    g_included = file;
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

static bool path_has_transversal(const char* filename) {
    if(filename[0] == '/' || filename[0] == '\\') return true;
    if(filename[0] != '\0' && filename[1] == ':') return true;

    return false;
}

static char* readfile(const char* path, Arena *arena) {
    FILE* f = fopen(path, "rb");
    if(!f) return NULL;

    if(fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }

    long len = ftell(f);

    if((size_t)len > MAX_FILE_SIZE) {
        fprintf(stderr, "Preprocessador: arquivo '%s' excede o tamanho maximo permitido "
            "(%zu MB)\n", path, (size_t)(MAX_FILE_SIZE / MB));
        fclose(f);
        exit(READFILE_ERROR);
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

static char* resolve_and_check_path(const char* root, const char* filename, Arena *arena) {
    if(path_has_transversal(filename)) {
        fprintf(stderr, "Preprocessador: path invalido em 'extract': '%s'. "
            "Paths absolutos ou com '..' nao sao permitidos.\n", filename);
        exit(PREPROCESS_ERROR);
    }

    return join_path(root, filename, arena);
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

    if(strncmp(peek, "target", 6) != 0 || isidentifier(peek[6])) {
        *out = source;
        return policy;
    }

    peek += 6;

    while(*peek == ' ' || *peek == '\t') peek++;
    if(*peek != '-' || *(peek + 1) != '>') {
        *out = source;
        return policy;
    }

    peek += 2;

    while(*peek == ' ' || *peek == '\t') peek++;
    if(strncmp(peek, "single", 6) == 0 && !isidentifier(peek[6])) {
        policy = INCLUDE_SINGLE;
        peek += 6;
    } else if(strncmp(peek, "multiple", 8) == 0 && !isidentifier(peek[8])) {
        policy = INCLUDE_MULTIPLE;
        peek += 8;
    } else {
        fprintf(stderr, "Preprocessador: argumento invalido em 'use target'. "
            "Use 'single' ou 'multiple'\n");
        exit(PREPROCESS_ERROR);
    }

    while(*peek && *peek != '\n') peek++;
    if(*peek == '\n') peek++;

    *out = peek;
    return policy;
}

static char* parse_extract_target(const char** src_ptr, Arena *arena, char* out) {
    const char* src = *src_ptr;
    char close;

    if(*src == '"') {
        close = '"';
        *out = 'p';
    } else if(*src == '[') {
        close = ']';
        *out = 'l';
    } else {
        fprintf(stderr, "Preprocessador: sintaxe invalida em 'extract'. "
            "Use '\"arquivo.lh\"' ou '[stdlib]'\n");
        exit(PREPROCESS_ERROR);
    }

    src++;
    const char* start = src;
    while(*src && *src != close && *src != '\n') src++;

    if(*src != close) {
        fprintf(stderr, "Preprocessador: delimitador '%c' nao fechado em 'extract'\n", close);
        exit(PREPROCESS_ERROR);
    }

    size_t len = (size_t)(src - start);
    src++;

    char* target = (char*)arena_alloc(arena, len + 1);
    memcpy(target, start, len);
    target[len] = '\0';

    *src_ptr = src;
    return target;
}

static bool file_declares_single(const char* src) {
    const char* peek = src;

    for(;;) {
        const char* line_start = peek;
        while(*peek == ' ' || *peek == '\t') peek++;

        if(*peek == '\n') {
            peek++;
            continue;
        }

        if(*peek == '\0') return false;

        if(strncmp(peek, "use", 3) == 0 && !isidentifier(peek[3])) {
            const char* pass_kw = peek + 3;
            while(*pass_kw == ' ' || *pass_kw == '\t') pass_kw++;

            const char* pass = { 0 };
            IncludePolicy policy = parse_use_directive(pass_kw, &pass);
            if(pass != pass_kw) return policy == INCLUDE_SINGLE;
        }
        (void)line_start;
        return false;
    }
}

static void expand_extracts(const char* source, const char* filename, const char* base_dir, const char* lib_dir,
    Arena *arena, int depth, LineMap *map, int *out_line, StringBuilder *sb, SourceRegisterFn register_fn, void *userdata) {
    
    linemap_push(map, arena, *out_line + 1, 1, filename);

    const char* src = source;
    int local_line = 1;

    while(*src) {
        const char* iter_start = src;
        while(*src == ' ' || *src == '\t') src++;

        bool handled = false;

        if(strncmp(src, "use", 3) == 0 && !isidentifier(src[3])) {
            const char* after_keyword = src + 3;
            while(*after_keyword == ' ' || *after_keyword == '\t') after_keyword++;

            const char* after = after_keyword;
            IncludePolicy parsed_policy = parse_use_directive(after_keyword, &after);
            (void)parsed_policy;

            if(after != after_keyword) {
                src = after;
                string_builder_putchar(sb, '\n');
                (*out_line)++;
                handled = true;
            }
        }

        if(!handled && strncmp(src, "extract", 7) == 0 && !isidentifier(src[7])) {
            const char* pass_kw = src + 7;
            while(*pass_kw == ' ' || *pass_kw == '\t') pass_kw++;

            const char* peek = pass_kw;
            if(*peek == '-' && *(peek + 1) == '>') peek += 2;
            while(*peek == ' ' || *peek == '\t') peek++;

            char type = 0;
            char* target_name = parse_extract_target(&peek, arena, &type);
            src = peek;

            const char* root_dir = (type == 'l') ? lib_dir : base_dir;
            char* resolved_path = resolve_and_check_path(root_dir, target_name, arena);
            char* file_src = readfile(resolved_path, arena);

            if(!file_src) {
                fprintf(stderr, "Preprocessador: arquivo nao encontrado: '%s' "
                                "(includado a partir de '%s', linha %d)\n",
                                resolved_path, filename, local_line);
                exit(PREPROCESS_ERROR);
            }

            if(register_fn) register_fn(userdata, resolved_path, file_src);

            char* new_base_dir = get_dir(resolved_path, arena);
            char* canonical = dedup(resolved_path, arena);

            while(*src && *src != '\n') src++;
            if(*src == '\n') src++;

            bool single_mode = file_declares_single(file_src);
            if(single_mode && included_check(canonical)) {
                string_builder_putchar(sb, '\n');
                (*out_line)++;
                handled = true;
            } else {
                if(single_mode) included_add(canonical);

                int line_before = local_line;

                preprocess_internal(file_src, resolved_path, new_base_dir, lib_dir, arena, depth + 1, map, out_line, sb, register_fn, userdata);

                if(sb->len == 0 || sb->data[sb->len - 1] != '\n') {
                    string_builder_putchar(sb, '\n');
                    (*out_line)++;
                }

                linemap_push(map, arena, *out_line + 1, line_before + 1, filename);
                handled = true;
            }
        }

        if(!handled) {
            src = iter_start;
            while(*src && *src != '\n') {
                string_builder_putchar(sb, *src);
                src++;
            }

            if(*src == '\n') {
                string_builder_putchar(sb, '\n');
                src++;
                (*out_line)++;
            }
        }

        for(const char* peek = iter_start; peek < src; peek++) 
            if(*peek == '\n') local_line++;
    }
}

static void preprocess_internal(const char* source, const char* filename, const char* base_dir, const char* lib_dir,
    Arena *arena, int depth, LineMap *map, int *out_line, StringBuilder *sb,
    SourceRegisterFn register_fn, void* userdata) {

    if(depth > MAX_INCLUDE_DEPTH) {
        fprintf(stderr,
            "Preprocessador: profundidade maxima de includes (%d) excedida "
            "ao processar '%s'.\nIsso normalmente indica um ciclo de "
            "includes (ex: A inclui B que inclui A de volta).\n",
            MAX_INCLUDE_DEPTH, filename);
        exit(PREPROCESS_ERROR);
    }

    expand_extracts(source, filename, base_dir, lib_dir, arena, depth, map, out_line, sb, register_fn, userdata);
}

static Macro* collect_macros(const char* source, char* out, Arena *arena) {
    Macro* first = NULL;
    const char* src = source;
    char* w = out;

    while(*src) {
        const char* line_start = src;
        while(*src == ' ' || *src == '\t') src++;

        const char* kw = "preprocess";
        size_t kw_len = 10;
        bool is_preprocess = (strncmp(src, kw, kw_len) == 0 && !isidentifier(src[kw_len]));

        if(is_preprocess) {
            src += kw_len;
            while(*src == ' ' || *src == '\t') src++;

            const char* start = src;
            while(isidentifier(*src) || isnumeric(*src)) src++;
            size_t len = (size_t)(src - start);

            if(len == 0) {
                fprintf(stderr, "Preprocessador: 'preprocess' sem nome de macro valido\n");
                exit(PREPROCESS_ERROR);
            }

            char* name_copy = (char*)arena_alloc(arena, len + 1);
            memcpy(name_copy, start, len);
            name_copy[len] = '\0';

            while(*src == ' ' || *src == '\t') src++;
            if(*src == '-' && *(src + 1) == '>') src += 2;
            while(*src == ' ' || *src == '\t') src++;

            const char* value_start = src;
            while(*src && *src != '\n') src++;
            size_t value_len = (size_t)(src - value_start);

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
            if(*src == '\n') src++;
        } else {
            src = line_start;
            while(*src && *src != '\n') *w++ = *src++;
            if(*src == '\n') *w++ = *src++;
        }
    }

    *w = '\0';
    return first;
}

static char* expand_macros(const char* source, Macro *macros, Arena *arena) {
    StringBuilder sb;
    string_builder_init(&sb, arena);

    const char* src = source;

    while(*src) {
        if(*src == '"') {
            const char* start = src;
            src++;

            while(*src && *src != '"') {
                if(*src == '\\' && *(src + 1)) src++;
                src++;
            }

            if(*src) src++;
            string_builder_write(&sb, start, (size_t)(src - start));
            continue;
        }

        if(*src == '\'') {
            const char* start = src;
            src++;

            while(*src && *src != '\'') {
                if(*src == '\\' && *(src + 1)) src++;
                src++;
            }

            if(*src) src++;
            string_builder_write(&sb, start, (size_t)(src - start));
            continue;
        }

        if(*src == '/' && *(src + 1) == '/') {
            const char* start = src;
            while(*src && *src != '\n') src++;
            string_builder_write(&sb, start, (size_t)(src - start));
            continue;
        }

        if(strncmp(src, "/comment", 8) == 0 && !isidentifier(src[8])) {
            src += 8;
            const char* end_mark = "endcomment/";
            size_t end_len = 11;
            int nl_comment = 0;

            while(*src && strncmp(src, end_mark, end_len) != 0) {
                if(*src == '\n') nl_comment++;
                src++;
            }

            if(*src) src += end_len;
            else {
                fprintf(stderr, "Preprocessador: comentario de bloco nao terminado "
                                "(esperado 'endcomment/')\n");
                exit(PREPROCESS_ERROR);
            }

            for(int i = 0; i < nl_comment; ++i) string_builder_putchar(&sb, '\n');
            continue;
        }

        if(isidentifier(*src) && !(*src >= '0' && *src <= '9')) {
            const char* start = src;
            while(isidentifier(*src)) src++;
            size_t len = (size_t)(src - start);

            Macro* found = NULL;
            for(Macro* macro = macros; macro != NULL; macro = macro->next) {
                if(macro->name_len == len && strncmp(macro->name, start, len) == 0) {
                    found = macro;
                    break;
                }
            }

            if(found) string_builder_write(&sb, found->value, found->value_len);
            else string_builder_write(&sb, start, len);
            continue;
        }

        string_builder_putchar(&sb, *src);
        src++;
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

char* preprocess_source(const char* src, const char* base_dir, const char* lib_dir, const char* filename, Arena *arena, LineMap *out,
    SourceRegisterFn register_fn, void *userdata) {
    included_init(arena);
    if(base_dir == NULL) base_dir = get_dir(filename, arena);

    if(register_fn) register_fn(userdata, filename, src);

    LineMap local_map;
    LineMap* map = out ? out : &local_map;

    linemap_init(map);

    StringBuilder sb;
    string_builder_init(&sb, arena);

    int out_line = 0;
    preprocess_internal(src, filename, base_dir, lib_dir, arena, 0, map, &out_line, &sb, register_fn, userdata);

    char* full_extract = string_builder_finish(&sb);

    size_t len = strlen(full_extract);
    char* stripped = (char*)arena_alloc(arena, len + 1);
    Macro* macros = collect_macros(full_extract, stripped, arena);

    if(!macros) return stripped;
    return expand_macros_recursive(stripped, macros, arena);
}
