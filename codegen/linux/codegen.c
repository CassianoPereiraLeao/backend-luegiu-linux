#include "codegen.h"

#define SPACES fprintf(out, "    ");
#define SPACESNL fprintf(out, "\n    ");
#define NL fprintf(out, "\n");

static int type_bytes_size(TypecheckType type) {
    if(type.ptr_lvl > 0) return 8;
    if(type.is_vla) return 8;
    if(type.inline_def != NULL) return (int)type.inline_def->size;
    switch (type.base)
    {
    case KINT8: case KUINT8: case KCHAR: return 1;
    case KINT16: case KUINT16: case KUTFCHAR: return 2;
    case KINT32: case KUINT32: case KFLOAT: return 4;
    case KINT64: case KUINT64: case KDOUBLE: case KHEXA: case KLINK: return 8;
    default: return 8;
    }
}

static bool view_equals(View a, View b) {
    if(a.len != b.len) return false;

    return memcmp(a.start, b.start, b.len) == 0;
}

static const char* register_name(RegFamily family, int size) {
    static const char* table[4][4] = {
        { "al", "ax", "eax", "rax" },
        { "cl", "cx", "ecx", "rcx" },
        { "dl", "dx", "edx", "rdx" },
        { "r11b", "r11w", "r11d", "r11" }
    };

    int index = 0;
    if(size == 2) index = 1;
    else if(size == 4) index = 2;
    else if(size == 8) index = 3;

    return table[family][index];
}

static int alignup(int value, int alignment) {
    if(alignment <= 1) return value;
    return (value + alignment - 1)& ~(alignment - 1); 
}

static void mark_size(CodegenSlotLayout *layouts, int id, TypecheckType type) {
    if(layouts[id].size == 0) {
        layouts[id].size = type_bytes_size(type);
    }
}

static void scan_value_for_layout(IrValue value, CodegenSlotLayout *slots, CodegenSlotLayout* temps) {
    if(value.kind == IR_VAL_SLOT) mark_size(slots, value.as.slot_id, value.type);
    else if(value.kind == IR_VAL_TEMP) mark_size(temps, value.as.temp_id, value.type);
}

static void compute_layouts(CodegenContext *ctx, IrGenContext *ir_ctx, size_t start, size_t end, int slot_count, int temp_count) {
    CodegenSlotLayout* slots = (CodegenSlotLayout*)arena_alloc(ctx->arena, sizeof(CodegenSlotLayout) * (slot_count > 0 ? slot_count : 1));
    CodegenSlotLayout* temps = (CodegenSlotLayout*)arena_alloc(ctx->arena, sizeof(CodegenSlotLayout) * (temp_count > 0 ? temp_count : 1));

    for(int i = 0; i < slot_count; ++i) slots[i].size = 0;
    for(int i = 0; i < temp_count; ++i) temps[i].size = 0;

    for(size_t i = start; i < end; ++i) {
        IrInstruction instruction = ir_ctx->instructions.items[i];
        scan_value_for_layout(instruction.dest, slots, temps);
        scan_value_for_layout(instruction.src1, slots, temps);
        scan_value_for_layout(instruction.src2, slots, temps);
        scan_value_for_layout(instruction.src3, slots, temps);
    }

    int offset = 0;
    for(int i = 0; i < slot_count; ++i) {
        int size = slots[i].size ? slots[i].size : 8;
        offset = alignup(offset, size) + size;
        slots[i].offset = -offset;
    }

    for(int i = 0; i < temp_count; ++i) {
        int size = temps[i].size ? temps[i].size : 8;
        offset = alignup(offset, size) + size;
        temps[i].offset = -offset;
    }

    ctx->slot_layouts = slots;
    ctx->temp_layouts = temps;
    ctx->frame_size = alignup(offset, 16);
}

static int slot_offset(CodegenContext *ctx, int slot_id) {
    return ctx->slot_layouts[slot_id].offset;
}

static int temp_offset(CodegenContext *ctx, int temp_id) {
    return ctx->temp_layouts[temp_id].offset;
}

static void emit_value_as_operand(FILE *out, CodegenContext *ctx, IrValue value) {
    switch (value.kind)
    {
    case IR_VAL_CONST_INT:  
        fprintf(out, "%lld", value.as.const_i);
        break;
    case IR_VAL_SLOT:
        fprintf(out, "[rbp%d]", slot_offset(ctx, value.as.slot_id) + value.field_offset);
        break;
    case IR_VAL_TEMP:
        fprintf(out, "[rbp%d]", temp_offset(ctx, value.as.temp_id) + value.field_offset);
        break;
    default:
        fprintf(out, "0");
        break;
    }
}

static const char* instruction_to_str(IrOperators op) {
    switch (op)
    {
    case IR_ADD: return "add";
    case IR_SUB: return "sub";
    case IR_MUL: return "imul";
    case IR_ATOMIC_AND:
    case IR_BAND: return "and";
    case IR_ATOMIC_OR:
    case IR_BOR: return "or";
    case IR_ATOMIC_XOR:
    case IR_BXOR: return "xor";
    default: return NULL;
    }
}

static bool type_is_signed(TypecheckType type) {
    if(type.ptr_lvl > 0) return false;
    switch(type.base) {
        case KINT8: case KINT16: case KINT32: case KINT64:
            return true;
        default: return false;
    }
}

static int value_size(CodegenContext *ctx, IrValue value) {
    (void)ctx;
    return type_bytes_size(value.type);
}

static const char* int_arg_registers(int index, int size) {
    static const char* r64[6] = { "rdi", "rsi", "rdx", "rcx", "r8", "r9" };
    static const char* r32[6] = { "edi", "esi", "edx", "ecx", "r8d", "r9d" };
    static const char* r16[6] = { "di", "si", "dx", "cx", "r8w", "r9w" };
    static const char* r8[6] = { "dil", "sil", "dl", "cl", "r8b", "r9b" };
    
    switch (size)
    {
    case 1: return r8[index];
    case 2: return r16[index];
    case 4: return r32[index];
    default: return r64[index];
    }
}

static void emit_load_to(FILE *out, CodegenContext *ctx, IrValue value, const char* dest_register, int dest_size) {
    (void)ctx;

    if(value.kind == IR_VAL_CONST_STRING) {
        fprintf(out, "lea %s, [rel Lstr%d]", dest_register, value.as.string_id);
        return;
    }

    if(value.kind == IR_VAL_CONST_INT) {
        fprintf(out, "mov %s, %lld", dest_register, value.as.const_i);
        return;
    }

    int source_size = value_size(ctx, value);

    if(source_size >= dest_size) {
        fprintf(out, "mov %s, ", dest_register);
        emit_value_as_operand(out, ctx, value);
        return;
    }

    bool signed_type = type_is_signed(value.type);

    if(source_size == 4 && dest_size == 8) {
        if(signed_type) {
            fprintf(out, "movsxd %s, dword ", dest_register);
            emit_value_as_operand(out, ctx, value);
        } else {
            fprintf(out, "mov %s, ", dest_register);
            emit_value_as_operand(out, ctx, value);
        }
        return;
    }

    const char* size_keyword = (source_size == 1) ? "byte" : "word";
    fprintf(out, "%s %s, %s ", signed_type ? "movsx" : "movzx", dest_register, size_keyword);
    emit_value_as_operand(out, ctx, value);
}

static void emit_load(FILE *out, CodegenContext *ctx, IrValue value, RegFamily family, int dest_size) {
    emit_load_to(out, ctx, value, register_name(family, dest_size), dest_size);
}

static int compute_width(TypecheckType type) {
    return type_bytes_size(type);
}

static void emit_store(FILE *out, CodegenContext *ctx, RegFamily family, IrValue dest) {
    int size = value_size(ctx, dest);
    fprintf(out, "mov ");
    emit_value_as_operand(out, ctx, dest);
    fprintf(out, ", %s", register_name(family, size));
}

static void emit_epilogue_label(FILE *out, int func_id) {
    fprintf(out, ".Lepi%d:", func_id); NL;
}

static void emit_jmp_epilogue(FILE *out, int func_id) {
    SPACES; fprintf(out, "jmp .Lepi%d", func_id); NL;
}

static void emit_func_prologue(CodegenContext *ctx, Node *func_node) {
    FILE *out = ctx->out;

    SPACES; fprintf(out, "push rbp"); NL;
    SPACES; fprintf(out, "mov rbp, rsp"); NL;

    if(ctx->frame_size > 0) {
        SPACES; fprintf(out, "sub rsp, %d", ctx->frame_size); NL;
    }

    NodeList* params = func_node->ast.decl_function.params;
    for(size_t i = 0; i < params->count && i < 6; ++i) {
        int size = ctx->slot_layouts[i].size;
        const char* reg = int_arg_registers((int)i, size);
        int offset = slot_offset(ctx, (int)i);
        SPACES; fprintf(out, "mov [rbp%d], %s", offset, reg); NL;
    }
}

static void emit_epilogue_function(FILE *out) {
    SPACES; fprintf(out, "leave"); NL;
    SPACES; fprintf(out, "ret"); NL;
}

static void emit_instruction(CodegenContext *ctx, IrInstruction instruction) {
    FILE* out = ctx->out;

    switch (instruction.op)
    {
    case IR_SLOT_DECL:
        return;

    case IR_LOAD_INDIRECT: {
        int width = compute_width(instruction.dest.type);
        SPACES; emit_load(out, ctx, instruction.src1, REG_RAX, 8); NL;
        SPACES;
        if(instruction.aux != 0) fprintf(out, "mov %s, [rax%+d]", register_name(REG_RCX, width), instruction.aux);
        else fprintf(out, "mov %s, [rax]", register_name(REG_RCX, width));
        NL;
        SPACES; emit_store(out, ctx, REG_RCX, instruction.dest); NL;
        return;
    }

    case IR_STORE_INDIRECT: {
        int width = compute_width(instruction.src2.type);
        SPACES; emit_load(out, ctx, instruction.src1, REG_RAX, 8); NL;
        SPACES; emit_load(out, ctx, instruction.src2, REG_RCX, width); NL;
        SPACES;
        if(instruction.aux != 0) fprintf(out, "mov [rax%+d], %s", instruction.aux, register_name(REG_RCX, width));
        else fprintf(out, "mov [rax], %s", register_name(REG_RCX, width));
        NL;
        return;
    }

    case IR_LABEL: {
        fprintf(out, ".L%d:", instruction.dest.as.label_id); NL;
        return;
    }
    
    case IR_JMP: {
        SPACES; fprintf(out, "jmp .L%d", instruction.src1.as.label_id); NL;
        return;
    }

    case IR_JMP_IF_ZERO: {
        int width = compute_width(instruction.src1.type);
        const char* acc = register_name(REG_RAX, width);
        SPACES; emit_load(out, ctx, instruction.src1, REG_RAX, width);
        SPACESNL; fprintf(out, "cmp %s, 0", acc); NL;
        SPACES; fprintf(out, "je .L%d", instruction.src2.as.label_id); NL;
        return;
    }

    case IR_ASSIGN: {
        int width = compute_width(instruction.src1.type);
        SPACES; emit_load(out, ctx, instruction.src1, REG_RAX, width);
        SPACESNL; emit_store(out, ctx, REG_RAX, instruction.dest); NL;
        return;
    }

    case IR_ADD: case IR_SUB: case IR_MUL: case IR_BAND: case IR_BOR: case IR_BXOR: {
        int width = compute_width(instruction.dest.type);
        const char* acc = register_name(REG_RAX, width);
        const char* cnt = register_name(REG_RCX, width);
        SPACES; emit_load(out, ctx, instruction.src1, REG_RAX, width);
        SPACESNL; emit_load(out, ctx, instruction.src2, REG_RCX, width);
        SPACESNL; fprintf(out, "%s %s, %s", instruction_to_str(instruction.op), acc, cnt); NL;
        SPACESNL; emit_store(out, ctx, REG_RAX, instruction.dest); NL;
        return;
    }

    case IR_DIV: case IR_MOD: {
        int width = compute_width(instruction.dest.type);
        const char* cnt = register_name(REG_RCX, width);
        SPACES; emit_load(out, ctx, instruction.src1, REG_RAX, width);
        SPACESNL; fprintf(out, "%s", width == 8 ? "cqo" : "cdq"); NL;
        SPACES; emit_load(out, ctx, instruction.src2, REG_RCX, width);
        emit_value_as_operand(out, ctx, instruction.src2);
        SPACESNL; fprintf(out, "idiv %s", cnt); NL;
        SPACES; emit_store(out, ctx, instruction.op == IR_DIV ? REG_RAX : REG_RDX, instruction.dest); NL;
        return;
    }

    case IR_SHL: case IR_SHR: {
        int width = compute_width(instruction.dest.type);
        const char* acc = register_name(REG_RAX, width);
        SPACES; emit_load(out, ctx, instruction.src1, REG_RAX, width);
        SPACESNL; emit_load(out, ctx, instruction.src2, REG_RCX, 4);
        SPACESNL; fprintf(out, "%s %s, cl", instruction.op == IR_SHL ? "shl" : "shr", acc); NL;
        SPACES; emit_store(out, ctx, REG_RAX, instruction.dest); NL;
        return;
    }

    case IR_CMP_LT: case IR_CMP_LE: case IR_CMP_GT: case IR_CMP_GE:
    case IR_CMP_EQ: case IR_CMP_NE: {
        int width = compute_width(instruction.src1.type);
        const char* acc = register_name(REG_RAX, width);
        const char* cnt = register_name(REG_RCX, width);

        const char* set;
        switch (instruction.op)
        {
        case IR_CMP_LT: set = "setl"; break;
        case IR_CMP_LE: set = "setle"; break;
        case IR_CMP_GT: set = "setg"; break;
        case IR_CMP_GE: set = "setge"; break;
        case IR_CMP_EQ: set = "sete"; break;
        default: set = "setne"; break;
        }

        SPACES; emit_load(out, ctx, instruction.src1, REG_RAX, width);
        SPACESNL; emit_load(out, ctx, instruction.src2, REG_RCX, width);
        SPACESNL; fprintf(out, "cmp %s, %s", acc, cnt);
        SPACESNL; fprintf(out, "%s al", set); NL;
        if(width > 1) {
            SPACES; fprintf(out, "movzx %s, al", acc); NL;
        }
        SPACES; emit_store(out, ctx, REG_RAX, instruction.dest); NL;
        return;
    }

    case IR_NEG: {
        int width = compute_width(instruction.src1.type);
        const char* acc = register_name(REG_RAX, width);
        SPACES; emit_load(out, ctx, instruction.src1, REG_RAX, width);
        SPACESNL; fprintf(out, "neg %s", acc); NL;
        SPACES; emit_store(out, ctx, REG_RAX, instruction.dest); NL;
        return;
    }

    case IR_NOT: {
        int width = compute_width(instruction.src1.type);
        const char* acc = register_name(REG_RAX, width);
        SPACES; emit_load(out, ctx, instruction.src1, REG_RAX, width);
        SPACESNL; fprintf(out, "cmp %s, 0", acc); NL;
        SPACES; fprintf(out, "sete al"); NL;
        if(width > 0) {
            SPACES; fprintf(out, "movzx %s, al", acc); NL;
        }
        SPACES; emit_store(out, ctx, REG_RAX, instruction.dest); NL;
        return;
    }

    case IR_RETURN: {
        if(instruction.src1.kind != IR_VAL_NONE) {
            int width = compute_width(instruction.src1.type);
            SPACES; emit_load(out, ctx, instruction.src1, REG_RAX, width); NL;
        }
        emit_jmp_epilogue(out, ctx->current_function_id);
        return;
    }

    case IR_ARG: {
        int normal_width = compute_width(instruction.src1.type);
        bool builtin_call = ctx->is_builtin_arg[ctx->current_instruc_index];
        int width = builtin_call ? 8 : normal_width;
        const char* reg = int_arg_registers(instruction.aux, width);
        SPACES; emit_load_to(out, ctx, instruction.src1, reg, width); NL;
        return;
    }

    case IR_CALL: {
        if(instruction.src1.kind == IR_VAL_BUILTIN) { SPACES; fprintf(out, "call __syscall_builtin"); NL; }
        else {
            View name = instruction.src1.func_name;
            SPACES; fprintf(out, "call %.*s", (int)name.len, name.start); NL;
        }
        SPACES; emit_store(out, ctx, REG_RAX, instruction.dest); NL;
        return;
    }

    case IR_ALLOCA: {
        int width = compute_width(instruction.src1.type);
        SPACES; emit_load(out, ctx, instruction.src1, REG_RAX, width);
        SPACESNL; fprintf(out, "add rax, 15"); NL;
        SPACES; fprintf(out, "and rax, -16"); NL;
        SPACES; fprintf(out, "sub rsp, rax"); NL;
        SPACES; fprintf(out, "mov ");
        emit_value_as_operand(out, ctx, instruction.dest);
        fprintf(out, ", rsp"); NL;
        return;
    }

    case IR_STACK_SAVE: {
        SPACES; fprintf(out, "mov ");
        emit_value_as_operand(out, ctx, instruction.dest);
        fprintf(out, ", rsp"); NL;
        return;
    }

    case IR_STACK_RESTORE: {
        SPACES; fprintf(out, "mov rsp, ");
        emit_value_as_operand(out, ctx, instruction.src1);
        NL;
        return;
    }

    case IR_ATOMIC_ADD: {
        int width = compute_width(instruction.src1.type);
        const char* cnt = register_name(REG_RCX, width);

        SPACES; emit_load(out, ctx, instruction.src2, REG_RCX, width);
        SPACESNL; fprintf(out, "lock xadd ");
        emit_value_as_operand(out, ctx, instruction.src1);
        fprintf(out, ", %s", cnt); NL;

        if(instruction.dest.kind != IR_VAL_NONE) {
            const char* new_cnt = register_name(REG_RAX, width);
            SPACES; emit_load(out, ctx, instruction.src2, REG_RAX, width);
            SPACESNL; fprintf(out, "add %s, %s", new_cnt, cnt); NL;
            SPACES; emit_store(out, ctx, REG_RAX, instruction.dest); NL;
        }
        return;
    }

    case IR_ATOMIC_AND: case IR_ATOMIC_OR: case IR_ATOMIC_XOR: {
        int width = compute_width(instruction.src1.type);
        const char* cnt = register_name(REG_RCX, width);

        SPACES; emit_load(out, ctx, instruction.src2, REG_RCX, width); NL;
        SPACES; fprintf(out, "lock %s ", instruction_to_str(instruction.op));
        emit_value_as_operand(out, ctx, instruction.src1);
        fprintf(out, ", %s", cnt); NL;

        if(instruction.dest.kind != IR_VAL_NONE) {
            SPACES; emit_load(out, ctx, instruction.src1, REG_RAX, width); NL;
            SPACES; emit_store(out, ctx, REG_RAX, instruction.dest); NL;
        }
        return;
    }

    case IR_FENCE: {
        SPACES; fprintf(out, "mfence"); NL;
        return;
    }

    case IR_ATOMIC_CAS: {
        int width = compute_width(instruction.src2.type);
        const char* cnt = register_name(REG_RCX, width);

        SPACES; emit_load(out, ctx, instruction.src1, REG_R11, 8); NL;
        SPACES; emit_load(out, ctx, instruction.src2, REG_RAX, width); NL;
        SPACES; emit_load(out, ctx, instruction.src3, REG_RCX, width); NL;
        SPACES; fprintf(out, "lock cmpxchg [r11], %s", cnt); NL;
        SPACES; fprintf(out, "sete al"); NL;
        SPACES; fprintf(out, "movzx eax, al"); NL;
        SPACES; emit_store(out, ctx, REG_RAX, instruction.dest); NL;
        return;
    }

    default:
        return;
    }
}

static void emit_function(CodegenContext *ctx, Node *func_node, IrGenContext *ir_ctx, IrFunctionRange range) {
    FILE* out = ctx->out;
    ctx->current_function_id = func_node->func_id;

    compute_layouts(ctx, ir_ctx, range.start, range.end, range.slot_count, range.temp_count);

    View name = func_node->ast.decl_function.name;

    if(!func_node->ast.decl_function.stattic && !view_equals(name, (View){"start", 5})) {
        fprintf(out, "global %.*s", (int)name.len, name.start); NL;
    }

    bool is_entry = (func_node == ctx->entry_function);
    if(is_entry) {
        fprintf(out, "start:"); NL;
    }

    fprintf(out, "%.*s:", (int)name.len, name.start); NL;

    emit_func_prologue(ctx, func_node);

    for(size_t i = range.start; i < range.end; ++i) {
        emit_instruction(ctx, ir_ctx->instructions.items[i]);
    }

    emit_epilogue_label(out, ctx->current_function_id);
    emit_epilogue_function(out);
}


CodegenContext create_codegen(FILE *out, Arena *arena, Node *func_entry, CheckContext *check_ctx) {
    CodegenContext ctx;
    ctx.out = out;
    ctx.arena = arena;
    ctx.entry_function = func_entry;
    ctx.check_ctx = check_ctx;
    ctx.current_function_id = 0;
    ctx.slot_layouts = NULL;
    ctx.temp_layouts = NULL;
    ctx.is_builtin_arg = NULL;
    ctx.current_instruc_index = 0;
    ctx.frame_size = 0;
    return ctx;
}

static bool* build_builtin_arg_map(Arena *arena, IrInstruction *items, size_t start, size_t end) {
    bool* is_builtin_arg = (bool*)arena_alloc(arena, sizeof(bool) * (end - start > 0 ? end - start : 1));

    bool pending_builtin = false;
    for(size_t i = end; i > start; --i) {
        size_t idx = i - 1;
        IrInstruction instr = items[idx];

        if(instr.op == IR_CALL) {
            pending_builtin = (instr.src1.kind == IR_VAL_BUILTIN);
            is_builtin_arg[idx] = false;
        } else if(instr.op == IR_ARG) {
            is_builtin_arg[idx] = pending_builtin;
        } else {
            pending_builtin = false;
            is_builtin_arg[idx] = false;
        }
    }

    return is_builtin_arg;
}

static void emit_externs(CodegenContext *ctx) {
    FILE* out = ctx->out;

    for(int i = 0; i < FUNC_TABLE_SIZE; ++i) {
        for(FuncEntry* entry = ctx->check_ctx->func_buckets[i]; entry != NULL; entry = entry->next) {
            if(entry->is_extern && !entry->has_body) {
                fprintf(out, "extern %.*s", (int)entry->name.len, entry->name.start); NL;
            }
        }
    }
}

void emit_program(CodegenContext *ctx, IrGenContext *ir_gen) {
    FILE* out = ctx->out;

    if(ir_gen->strings.count > 0) {
        fprintf(out, "section .data"); NL;
        for(size_t i = 0; i < ir_gen->strings.count; ++i) {
            IrStringEntry* entry = &ir_gen->strings.items[i];
            SPACES; fprintf(out, "Lstr%d: db ", entry->id);
            for(size_t j = 0; j < entry->text.len; ++j) {
                unsigned char c = (unsigned char)entry->text.start[j];

                if(c == '"') continue;

                if(c == '\\') {
                    j++;

                    switch(entry->text.start[j]) {
                        case 'n': c = 10; break;
                        case 't': c = 9; break;
                        case 'r': c = 13; break;
                        case '\\': c = 92; break;
                        case '"': c = 32; break;
                        default: --j; break;
                    }
                }

                fprintf(out, "%d, ", c);
            }

            fprintf(out, "0"); NL;
        }

        NL;
    }

    emit_externs(ctx);

    fprintf(out, "section .text"); NL;

    bool is_entry_unit = (ctx->entry_function != NULL);

    if(is_entry_unit) {
        fprintf(out, "global __syscall_builtin"); NL;
        fprintf(out, "global _start"); NL;
    } else {
        fprintf(out, "extern __syscall_builtin"); NL;
    }

    NL;

    ctx->is_builtin_arg = build_builtin_arg_map(ctx->arena, ir_gen->instructions.items, 0, ir_gen->instructions.count);

    for(size_t i = 0; i < ir_gen->functions.count; ++i) {
        IrFunctionRange range = ir_gen->functions.items[i];
        emit_function(ctx, range.func_node, ir_gen, range);
        NL;
    }

    if(is_entry_unit) {
        NL; fprintf(out, "__syscall_builtin:"); NL;
        SPACES; fprintf(out, "mov r11, rcx"); NL;
        SPACES; fprintf(out, "mov rax, rdi"); NL;
        SPACES; fprintf(out, "mov rdi, rsi"); NL;
        SPACES; fprintf(out, "mov rsi, rdx"); NL;
        SPACES; fprintf(out, "mov rdx, r11"); NL;
        SPACES; fprintf(out, "mov r11, r8"); NL;
        SPACES; fprintf(out, "mov r10, r11"); NL;
        SPACES; fprintf(out, "mov r8, r9"); NL;
        SPACES; fprintf(out, "mov r9, [rsp+8]"); NL;
        SPACES; fprintf(out, "syscall"); NL;
        SPACES; fprintf(out, "ret"); NL;

        NL; fprintf(out, "_start:"); NL;
        SPACES; fprintf(out, "mov edi, [rsp]"); NL;
        SPACES; fprintf(out, "lea rsi, [rsp+8]"); NL;
        SPACES; fprintf(out, "call start"); NL;
        SPACES; fprintf(out, "mov edi, eax"); NL;
        SPACES; fprintf(out, "mov eax, 60"); NL;
        SPACES; fprintf(out, "syscall"); NL;
    }
}
