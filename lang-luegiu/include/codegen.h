#pragma once

#include "common.h"
#include "string_view_helper.h"
#include "ir.h"

typedef enum {
    REG_RAX, REG_RCX, REG_RDX, REG_R11,
    REG_XMM0, REG_XMM1, REG_XMM2, REG_XMM3, REG_XMM4, REG_XMM5
} RegFamily;

typedef enum {
    R8,
    R16,
    R32,
    R64,
} ArgFamily;

typedef struct {
    int offset;
    int size;
} CodegenSlotLayout;

typedef struct {
    FILE* out;
    Arena* arena;
    Node* entry;
    CheckContext* check_ctx;
    int current_func_id;
    CodegenSlotLayout* slot_layouts;
    CodegenSlotLayout* temp_layouts;
    int frame_size;
    bool* is_builtin_arg;
    size_t current_instruc_index;
} CodegenContext;

CodegenContext create_codegen(FILE* out, Arena *arena, Node *entry, CheckContext *check_ctx);
void emit_program(CodegenContext *ctx, IrGenContext *ir_ctx);
