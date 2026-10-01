#include "../../include/typecheck.h"

uint64_t view_hash(View view) {
    uint64_t hash = 14695981039346656037ULL;

    for(size_t i = 0; i < view.len; ++i) {
        hash ^= (unsigned char)view.start[i];
        hash *= 1099511628211ULL;
    }

    return hash;
}

#define DEFINE_HASH_TABLE(TableType, EntryType, ALIAS, INIT_SIZE) \
    static void ALIAS##_init(Arena *arena, TableType* table) {  \
        table->capacity = (INIT_SIZE); \
        table->count = 0;       \
        table->items = (EntryType**)arena_alloc(arena, sizeof(EntryType*) * (INIT_SIZE)); \
        memset(table->items, 0, sizeof(EntryType*) * (INIT_SIZE)); \
    } \
    \
    static void ALIAS##_rehash(Arena *arena, TableType* table) { \
        size_t new_cap = table->capacity * 2;                   \
        EntryType** new_items = (EntryType**)arena_alloc(arena, sizeof(EntryType*) * new_cap);   \
        memset(new_items, 0, sizeof(EntryType*) * new_cap); \
        \
        for(size_t i = 0; i < table->capacity; ++i) { \
            EntryType* entry = table->items[i]; \
            while(entry) {  \
                EntryType* next = entry->next; \
                uint64_t index = view_hash(entry->name) % new_cap; \
                entry->next = new_items[index]; \
                new_items[index] = entry; \
                entry = next; \
            }   \
        } \
        table->items = new_items; \
        table->capacity = new_cap; \
    } \
    \
    static EntryType* ALIAS##_insert(Arena *arena, TableType *table, View name) {    \
        if(table->capacity == 0) { \
            ALIAS##_init(arena, table); \
        } else if((double)(table->count + 1) / (double)table->capacity >= TABLE_LOAD_FACTOR) { \
            ALIAS##_rehash(arena, table); \
        } \
        EntryType* entry = (EntryType*)arena_alloc(arena, sizeof(EntryType)); \
        memset(entry, 0, sizeof(EntryType)); \
        entry->name = name; \
        uint64_t index = view_hash(name) % table->capacity; \
        entry->next = table->items[index]; \
        table->items[index] = entry; \
        table->count++; \
        return entry; \
    } \
    \
    static EntryType* ALIAS##_lookup(TableType *table, View name) { \
        if(table->capacity == 0) return NULL; \
        uint64_t index = view_hash(name) % table->capacity; \
        EntryType* entry = table->items[index]; \
        while(entry) {  \
            if(view_equals_view(entry->name, name)) return entry; \
            entry = entry->next; \
        } \
        return NULL; \
    } \
    \
    static EntryType* ALIAS##_declare(Arena *arena, TableType *table, View name) {  \
        if(ALIAS##_lookup(table, name) != NULL) return NULL; \
        return ALIAS##_insert(arena, table, name); \
    }

DEFINE_HASH_TABLE(SymbolTable, SymbolEntry, symtab, SCOPE_TABLE_INIT_SIZE)
DEFINE_HASH_TABLE(FuncTable, FuncEntry, functab, DEFAULT_INIT_SIZE)
DEFINE_HASH_TABLE(TypeTable, TypeEntry, typetab, DEFAULT_INIT_SIZE)
DEFINE_HASH_TABLE(GlobalTable, GlobalEntry, globaltab, DEFAULT_INIT_SIZE)
DEFINE_HASH_TABLE(LabelTable, LabelEntry, labeltab, DEFAULT_INIT_SIZE)
DEFINE_HASH_TABLE(NewtypeTable, TypeEntry, newtypetab, DEFAULT_INIT_SIZE)

#undef DEFINE_HASH_TABLE

static Arena* typecheck_arena = NULL;
static FuncTable g_functions = { 0 };
static TypeTable g_types = { 0 };
static GlobalTable g_globals = { 0 };
static LabelTable g_labels = { 0 };
static NewtypeTable g_newtypes = { 0 };

void typecheck_globals_init(Arena *arena) {
    typecheck_arena = arena;
    memset(&g_types, 0, sizeof(g_types));
    memset(&g_globals, 0, sizeof(g_globals));
    memset(&g_labels, 0, sizeof(g_labels));
    memset(&g_newtypes, 0, sizeof(g_newtypes));
    typetab_init(arena, &g_types);
    globaltab_init(arena, &g_globals);
    labeltab_init(arena, &g_labels);
    memset(&g_functions, 0, sizeof(g_functions));
    functab_init(arena, &g_functions);
}

Scope* scope_push(Arena *arena, Scope *parent) {
    Scope* scope = (Scope*)arena_alloc(arena, sizeof(Scope));
    symtab_init(arena, &scope->symbols);
    scope->next = parent;
    return scope;
}

SymbolEntry* scope_insert(Arena *arena, Scope *scope, View name, Typecheck type) {
    SymbolEntry* entry = symtab_declare(arena, &scope->symbols, name);
    if(!entry) return NULL;
    entry->type = type;
    return entry;
}

SymbolEntry* scope_lookup(Scope *scope, View name) {
    return symtab_lookup(&scope->symbols, name);
}

SymbolEntry* scope_lookup_chain(Scope *scope, View name) {
    while(scope) {
        SymbolEntry* found = symtab_lookup(&scope->symbols, name);
        if(found) return found;
        scope = scope->next;
    }

    return NULL;
}

FuncEntry* function_table_insert(View name) {
    return functab_declare(typecheck_arena, &g_functions, name);
}

FuncEntry* function_table_lookup(View name) {
    return functab_lookup(&g_functions, name);
}

void function_table_foreach(FuncTableVisitor visitor, void *userdata) {
    for(size_t i = 0; i < g_functions.capacity; ++i) {
        for(FuncEntry* entry = g_functions.items[i]; entry != NULL; entry = entry->next) {
            visitor(entry, userdata);
        }
    }
}

TypeEntry* type_table_insert(View name) {
    return typetab_declare(typecheck_arena, &g_types, name);
}

TypeEntry* type_table_lookup(View name) {
    return typetab_lookup(&g_types, name);
}


TypeEntry* newtype_table_insert(View name) {
    return newtypetab_declare(typecheck_arena, &g_newtypes, name);
}

TypeEntry* newtype_table_lookup(View name) {
    return newtypetab_lookup(&g_newtypes, name);
}

GlobalEntry* global_var_decl(View name, Typecheck type, Node *decl_node, bool _static, bool _extern, bool has_initializer) {
    GlobalEntry* entry = globaltab_declare(typecheck_arena, &g_globals, name);
    if(!entry) return NULL;
    entry->type = type;
    entry->decl_node = decl_node;
    entry->_static = _static;
    entry->_extern = _extern;
    entry->has_init = has_initializer;
    return entry;
}

GlobalEntry* global_var_lookup(View name) {
    return globaltab_lookup(&g_globals, name);
}

LabelEntry* label_declare(View name, Node *declare) {
    LabelEntry* exist = labeltab_lookup(&g_labels, name);

    if(exist) {
        if(exist->_defined) return NULL;

        exist->_defined = true;
        exist->decl_node = declare;
        return exist;
    }

    LabelEntry* entry = labeltab_insert(typecheck_arena, &g_labels, name);
    entry->_defined = true;
    entry->decl_node = declare;
    entry->first_jump = NULL;
    return entry;
}

LabelEntry* label_reference(View name, Node *use) {
    LabelEntry* entry = labeltab_lookup(&g_labels, name);

    if(!entry) {
        entry = labeltab_insert(typecheck_arena, &g_labels, name);
        entry->_defined = false;
        entry->decl_node = NULL;
        entry->first_jump = use;
    }

    return entry;
}

void label_table_foreach(LabelTableVisitor visitor, void* userdata) {
    for (size_t i = 0; i < g_labels.capacity; i++) {
        for (LabelEntry* entry = g_labels.items[i]; entry != NULL; entry = entry->next) {
            visitor(entry, userdata);
        }
    }
}

bool resolve_variable(Scope *scope, View name, Typecheck *out) {
    SymbolEntry* local = scope_lookup_chain(scope, name);
    if(local) {
        *out = local->type;
        return true;
    }

    GlobalEntry* global = global_var_lookup(name);
    if(global) {
        *out = global->type;
        return true;
    }

    return false;
}

size_t func_count() {
    return g_functions.capacity;
}

FuncEntry* func_item(size_t i) {
    return g_functions.items[i];
}

static void report_pending_func(FuncEntry *entry, void *userdata) {
    CheckContext* ctx = (CheckContext*)userdata;

    if(entry->_pending) {
        Node* node = entry->decl_node;
        diag_error(ctx->ctx, node->filename, node->line, node->col,
            "A funcao '%.*s' foi declarada mas nunca definida", 
                (int)entry->name.len, entry->name.start);
    }
}

static void check_unresolved_foreward(CheckContext *ctx) {
    function_table_foreach(report_pending_func, ctx);
}

void typecheck_finalize(CheckContext *ctx) {
    check_unresolved_foreward(ctx);
}

CheckContext create_checkctx(Arena *arena, DiagContext *ctx) {
    CheckContext check = { 0 };
    check.arena = arena;
    check.current_scope = scope_push(arena, NULL);
    check.ctx = ctx;
    typecheck_globals_init(arena);
    return check;
}
