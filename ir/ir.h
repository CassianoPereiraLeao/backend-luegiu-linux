#pragma once

#include "../common.h"
#include "../parser/parser.h"
#include "../parser/typecheck.h"

#define IR_SCOPE_TABLE_SIZE 32
#define IR_FUNC_TABLE_SIZE 32

typedef enum {
    IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_MOD,
    IR_BAND, IR_BOR, IR_BXOR, IR_SHL, IR_SHR,
    IR_NEG, IR_NOT,
    IR_CMP_LT, IR_CMP_LE, IR_CMP_GT, IR_CMP_GE, IR_CMP_EQ, IR_CMP_NE,
    IR_ASSIGN,
    IR_LABEL,
    IR_JMP,
    IR_JMP_IF_ZERO,
    IR_RETURN, IR_ARG, IR_CALL, IR_ARG_STACK,
    IR_SLOT_DECL, IR_LOAD_INDIRECT, IR_STORE_INDIRECT,
    IR_ALLOCA, IR_STACK_SAVE, IR_STACK_RESTORE
} IrOperators;

typedef enum {
    IR_VAL_NONE,
    IR_VAL_TEMP,
    IR_VAL_CONST_INT,
    IR_VAL_CONST_FLOAT,
    IR_VAL_CONST_STRING,
    IR_VAL_SLOT,
    IR_VAL_LABEL,
    IR_VAL_FUNC,
    IR_VAL_BUILTIN
} IrValueKind;

typedef struct {
    IrValueKind kind;

    union {
        int temp_id;
        int slot_id;
        int func_id;
        long long const_i;
        double const_f;
        int label_id;
        int string_id;
    } as;

    TypecheckType type;
    int field_offset;
} IrValue;

typedef struct {
    IrOperators op;
    IrValue dest;
    IrValue src1;
    IrValue src2;
    int aux;
} IrInstruction;

typedef struct {
    IrInstruction* items;
    size_t count;
    size_t capacity;
} IrInstructionList;

typedef struct {
    Node* func_node;
    size_t start;
    size_t end;
    int slot_count;
    int temp_count;
} IrFunctionRange;

typedef struct {
    IrFunctionRange* items;
    size_t count;
    size_t capacity;
} IrFunctionRangeList;

typedef struct IrSlotEntry {
    View name;
    int slot_id;
    TypecheckType type;
    struct IrSlotEntry* next;

    bool is_array;
    size_t dim_count;
    IrValue* dim_strides;
} IrSlotEntry;

typedef struct IrFuncIdEntry {
    View name;
    int func_id;
    struct IrFuncIdEntry* next;
} IrFuncIdEntry;

typedef struct IrGenScope {
    IrSlotEntry* buckets[IR_SCOPE_TABLE_SIZE];
    struct IrGenScope* parent;
} IrGenScope;

typedef struct {
    View text;
    int id;
} IrStringEntry;

typedef struct {
    IrStringEntry* items;
    size_t count;
    size_t capacity;
} IrStringPool;

typedef struct {
    Arena* arena;
    IrInstructionList instructions;
    IrFunctionRangeList functions;
    IrFuncIdEntry* func_id_buckets[IR_FUNC_TABLE_SIZE];
    int next_temp;
    int next_slot;
    int next_label;
    int next_func_id;
    int loop_start_label;
    int loop_end_label;
    bool in_loop;
    IrGenScope* current_scope;
    IrStringPool strings;
} IrGenContext;

IrGenContext create_irgen_context(Arena *arena);
void irgen_program(IrGenContext *ctx, Node *program);
