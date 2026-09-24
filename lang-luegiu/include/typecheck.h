#pragma once

#include "types.h"
#include "parser.h"
#include "common.h"
#include "diagnostic.h"
#include "string_view_helper.h"

#define SCOPE_TABLE_INIT_SIZE 10
#define DEFAULT_INIT_SIZE 32
#define TABLE_LOAD_FACTOR 0.8

#define TABLE_DEF(T) \
    struct { \
        T items; \
        size_t count; \
        size_t capacity; \
    }

typedef struct AggregateDef AggregateDef;

typedef enum {
    COMPAT_OK,
    COMPAT_ERROR,
    COMPAT_WARNING
} CompatResult;

typedef enum {
    TYPE_ENTRY_ALIAS,
    TYPE_ENTRY_AGGREGATE,
    TYPE_ENTRY_ENUM,
} TypeEntryKind;

typedef struct {
    View name;
    Typecheck type;
    size_t offset;
    size_t size;
} FieldInfo;

struct AggregateDef {
    NodeKind kind;
    FieldInfo* fields;
    size_t count;
    size_t size;
};

typedef struct {
    LiteralKind kind;
    View name;
    unsigned long long ivalue;
} EnumMemberInfo;

typedef struct {
    EnumMemberInfo* members;
    size_t member_count;
} EnumDef;

typedef struct TypeEntry {
    View name;
    TypeEntryKind kind;

    union {
        Typecheck alias;
        AggregateDef aggregate;
        EnumDef enumdef;
    };

    struct TypeEntry* next;
} TypeEntry;

typedef struct {
    Typecheck call_type;
    Typecheck* param_types;
    size_t param_count;
    bool variadic;
} FunctionSignature;

typedef struct FuncEntry {
    View name;
    FunctionSignature signature;
    bool _static;
    bool _extern;
    bool _pending;
    bool has_body;
    Node* decl_node;
    struct FuncEntry* next;
} FuncEntry;

typedef struct SymbolEntry {
    View name;
    Typecheck type;
    struct SymbolEntry* next;
} SymbolEntry;

typedef struct GlobalEntry {
    View name;
    Typecheck type;
    bool _static;
    bool _extern;
    bool has_init;
    Node* decl_node;
    struct GlobalEntry* next;
} GlobalEntry;

typedef struct LabelEntry {
    View name;
    Node* first_jump;
    bool _defined;
    Node* decl_node;
    struct LabelEntry* next;
} LabelEntry;

typedef struct {
    View name;
    Typecheck type;
} Symbol;

typedef TABLE_DEF(SymbolEntry**) SymbolTable;
typedef TABLE_DEF(FuncEntry**) FuncTable;
typedef TABLE_DEF(TypeEntry**) TypeTable;
typedef TABLE_DEF(GlobalEntry**) GlobalTable;
typedef TABLE_DEF(LabelEntry**) LabelTable;
typedef TABLE_DEF(TypeEntry**) NewtypeTable;

#undef TABLE_DEF

typedef struct Scope {
    SymbolTable symbols;
    struct Scope* next;
} Scope;

typedef struct {
    Arena* arena;
    DiagContext *ctx;
    Scope* current_scope;
    Typecheck current_call_type;
    bool in_function;
    bool in_loop;
    Node* entry_function;
} CheckContext;

CheckContext create_checkctx(Arena *arena, DiagContext *ctx);
void check_program(CheckContext *ctx, Node *program);
void typecheck_finalize(CheckContext *ctx);
void typecheck_session_init(Arena *arena);

void typecheck_globals_init(Arena *arena);
Scope* scope_push(Arena *arena, Scope *parent);

SymbolEntry* scope_insert(Arena *arena, Scope *scope, View name, Typecheck type);
SymbolEntry* scope_lookup(Scope *scope, View name);
SymbolEntry* scope_lookup_chain(Scope *scope, View name);

FuncEntry* function_table_insert(View name);
FuncEntry* function_table_lookup(View name);

typedef void (*FuncTableVisitor)(FuncEntry *entry, void *userdata);
void function_table_foreach(FuncTableVisitor visitor, void *userdata);

TypeEntry* type_table_insert(View name);
TypeEntry* type_table_lookup(View name);

TypeEntry* newtype_table_insert(View name);
TypeEntry* newtype_table_lookup(View name);

GlobalEntry* global_var_decl(View name, Typecheck type, Node *decl_node, bool _static, bool _extern, bool has_initializer);
GlobalEntry* global_var_lookup(View name);

typedef void (*LabelTableVisitor)(LabelEntry *entry, void *userdata);
LabelEntry* label_declare(View name, Node *declare);
LabelEntry* label_reference(View name, Node *use);
void label_table_foreach(LabelTableVisitor visitor, void *userdata);

bool resolve_variable(Scope *scope, View name, Typecheck *out);
uint64_t view_hash(View view);

size_t func_count();
FuncEntry* func_item(size_t i);

bool isarith_compoundop(TokenType op);
bool isbitwise_compoundop(TokenType op);
