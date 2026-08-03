#pragma once

#include "../../common.h"
#include "../../parser/parser.h"
#include "../../ir/ir.h"

typedef enum {
    REG_RAX,
    REG_RCX,
    REG_RDX,
    REG_R11
} RegFamily;

typedef struct {
    int offset;
    int size;
} CodegenSlotLayout;

typedef struct {
    FILE* out;
    Arena *arena;
    Node* entry_function;
    CheckContext* check_ctx;
    int current_function_id;
    CodegenSlotLayout* slot_layouts;
    CodegenSlotLayout* temp_layouts;
    int frame_size;
    bool* is_builtin_arg;
    size_t current_instruc_index;
} CodegenContext;

CodegenContext create_codegen(FILE *out, Arena *arena, Node *func_entry, CheckContext *check_ctx);
void emit_program(CodegenContext *ctx, IrGenContext *ir_gen);
