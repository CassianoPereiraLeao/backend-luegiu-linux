#pragma once

#include "parser.h"

#define SCOPE_TABLE_SIZE 64
#define FUNC_TABLE_SIZE 64
#define TYPE_TABLE_SIZE 64

typedef struct AggregateDef AggregateDef;

typedef enum {
    COMPAT_OK,
    COMPAT_WARNING,
    COMPAT_ERROR
} CompatResult;

typedef enum {
    TYPE_ENTRY_ALIAS,
    TYPE_ENTRY_AGGREGATE,
    TYPE_ENTRY_ENUM
} TypeEntryKind;

typedef struct {
    View name;
    TypecheckType type;
    size_t offset;
    size_t size;
} FieldInfo;

struct AggregateDef {
    NodeKind kind;
    FieldInfo* fields;
    size_t field_count;
    size_t size;
};

typedef struct {
    View name;
    long long value;
} EnumMemberInfo;

typedef struct {
    EnumMemberInfo* members;
    size_t member_count;
} EnumDef;

typedef struct TypeEntry {
    View name;
    TypeEntryKind kind;
    TypecheckType alias;
    AggregateDef aggregate;
    EnumDef enum_def;
    struct TypeEntry* next;
} TypeEntry;

typedef struct {
    TypecheckType call_type;
    TypecheckType* param_types;
    size_t param_count;
    bool variadic;
} FuncSignature;

typedef struct FuncEntry {
    View name;
    FuncSignature signature;
    bool is_static;
    bool is_extern;
    bool is_pending;
    bool has_body;
    Node* decl_node;
    struct FuncEntry* next;
} FuncEntry;

typedef struct SymbolEntry {
    View name;
    TypecheckType type;
    struct SymbolEntry* next;
} SymbolEntry;

typedef struct {
    View name;
    TypecheckType type;
} Symbol;

typedef struct Scope {
    SymbolEntry* buckets[SCOPE_TABLE_SIZE];
    struct Scope* parent;
} Scope;

typedef struct {
    Arena* arena;
    DiagContext* context;
    Scope* current_scope;
    TypecheckType current_call_type;
    FuncEntry* func_buckets[FUNC_TABLE_SIZE];
    TypeEntry* type_buckets[TYPE_TABLE_SIZE];
    bool in_function;
    bool in_loop;
    Node* entry_function;
} CheckContext;

CheckContext create_check_context(Arena *arena, DiagContext *ctx);
void check_program(CheckContext *ctx, Node *program);
