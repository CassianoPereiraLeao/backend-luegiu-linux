#include "../../include/codegen.h"

static int type_bytes_size(Typecheck type) {
    if(type.ptr_lvl > 0) return 8;
    if(type._vla) return 8;
    if(type.inline_def != NULL) return (int)type.inline_def->size;

    switch (type.base)
    {
    case KINT8: case KUINT8: case KCHAR: return 1;
    case KINT16: case KUINT16: return 2;
    case KINT32: case KUINT32: case KFLOAT: return 4;
    case KINT64: case KUINT64: case KDOUBLE: return 8;
    default: return 8;
    }
}

static int width_to_family(int size) {
    switch (size) {
        case 1: return 3;
        case 2: return 2;
        case 4: return 1;
        default: return 0;
    }
}

static bool type_isfloat(Typecheck type) {
    if(type.ptr_lvl > 0) return false;
    return type.base == KFLOAT || type.base == KDOUBLE;
}

static const char* float_suffix(int size) {
    return size == 4 ? "ss" : "sd";
}

static const char* ireg_name(RegFamily family, int size) {
    const char* table[4][4] = {
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

static const char* iarg_registers(ArgFamily family, int index) {
    const char* table[4][6] = {
        { "rdi", "rsi", "rdx", "rcx", "r8", "r9" },
        { "edi", "esi", "edx", "ecx", "r8d", "r9d" },
        { "di", "si", "dx", "cx", "r8w", "r9w" },
        { "dil", "sil", "dl", "cl", "r8b", "r9b" }
    };

    return table[family][index];
}

static const char* freg_name(RegFamily family) {
    const char* xmm[6] = { "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5" };

    if(family <= REG_R11) return "none";

    return xmm[family - REG_XMM0];
}

static const char* farg_name(RegFamily family) {
    return freg_name(family);
}

static void mark_size(CodegenSlotLayout *layouts, int id, Typecheck type) {
    if(layouts[id].size == 0) {
        layouts[id].size = type_bytes_size(type);
    }
}

static void scan_value_for_layout(IrValue value, CodegenSlotLayout *slots, CodegenSlotLayout *temps) {
    if(value.kind == IR_VAL_SLOT) mark_size(slots, value.as.slot_id, value.type);
    else if(value.kind == IR_VAL_TEMP) mark_size(temps, value.as.temp_id, value.type);
}

static void compute_layouts(CodegenContext *ctx, IrGenContext *ir_ctx, size_t start, size_t end, int scount, int tcount) {
    CodegenSlotLayout* slots = (CodegenSlotLayout*)arena_alloc(ctx->arena, sizeof(CodegenSlotLayout) * (scount > 0 ? scount : 1));
    CodegenSlotLayout* temps = (CodegenSlotLayout*)arena_alloc(ctx->arena, sizeof(CodegenSlotLayout) * (tcount > 0 ? tcount : 1));

    for(int i = 0; i < scount; ++i) slots[i].size = 0;
    for(int i = 0; i < tcount; ++i) temps[i].size = 0;

    for(size_t i = start; i < end; ++i) {
        IrInstruction instruction = ir_ctx->instructions.items[i];
        scan_value_for_layout(instruction.dest, slots, temps);
        scan_value_for_layout(instruction.src1, slots, temps);
        scan_value_for_layout(instruction.src2, slots, temps);
        scan_value_for_layout(instruction.src3, slots, temps);
    }

    int offset = 0;
    for(int i = 0; i < scount; ++i) {
        int size = slots[i].size ? slots[i].size : 8;
        offset = align_size(offset, size) + size;
        slots[i].offset = -offset;
    }

    for(int i = 0; i < tcount; ++i) {
        int size = temps[i].size ? temps[i].size : 8;
        offset = align_size(offset, size) + size;
        temps[i].offset = -offset;
    }

    ctx->slot_layouts = slots;
    ctx->temp_layouts = temps;
    ctx->frame_size = align_size(offset, 16);
}

static int slot_offset(CodegenContext *ctx, int id) {
    return ctx->slot_layouts[id].offset;
}

static int temp_offset(CodegenContext *ctx, int id) {
    return ctx->temp_layouts[id].offset;
}

static void emit_value_as_op(FILE *out, CodegenContext *ctx, IrValue value) {
    switch (value.kind)
    {
    case IR_VAL_CONST_INT:
        fprintf(out, "%lld", value.as.const_i);
        break;
    case IR_VAL_CONST_UINT:
        fprintf(out, "%zu", value.as.const_ui);
        break;
    case IR_VAL_CONST_FLOAT:
        fprintf(out, "%lf", value.as.const_f);
        break;
    case IR_VAL_SLOT:
        int soffset = slot_offset(ctx, value.as.slot_id) + value.field_offset;
        if(soffset == 0) fprintf(out, "[rbp+0]");
        else fprintf(out, "[rbp%d]", soffset);
        break;
    case IR_VAL_TEMP:
        int toffset = temp_offset(ctx, value.as.temp_id) + value.field_offset;
        if(toffset == 0) fprintf(out, "[rbp+0]");
        else fprintf(out, "[rbp%d]", toffset);
        break;
    default:
        fprintf(out, "0");
        break;
    }
}

static const char* instruction_to_str(IrOperators op) {
    switch (op)
    {
    case IR_ATOMIC_ADD:
    case IR_ADD: return "add";
    case IR_SUB: return "sub";
    case IR_MOD:
    case IR_DIV: return "idiv";
    case IR_MUL: return "imul";
    case IR_ATOMIC_OR:
    case IR_BOR: return "or";
    case IR_ATOMIC_AND:
    case IR_BAND: return "and";
    case IR_ATOMIC_XOR:
    case IR_BXOR: return "xor";
    default: return NULL;
    }
}

static bool issigned(Typecheck type) {
    if(type.ptr_lvl > 0) return false;
    switch (type.base)
    {
    case KINT8: case KINT16: case KINT32: case KINT64:
        return true;
    default:
        return false;
    }
}

static int value_size(IrValue value) {
    return  type_bytes_size(value.type);
}

static void emit_load_to(FILE *out, CodegenContext *ctx, IrValue value, const char* dest_reg, int dest_size) {
    if(value.kind == IR_VAL_CONST_STRING) {
        fprintf(out, "lea %s, [rel Lstr%d]", dest_reg, value.as.string_id);
        return;
    }

    if(value.kind == IR_VAL_CONST_INT) {
        fprintf(out, "mov %s, %lld", dest_reg, value.as.const_i);
        return;
    }

    if(value.kind == IR_VAL_CONST_UINT) {
        fprintf(out, "mov %s, %zu", dest_reg, value.as.const_ui);
        return;
    }

    int src_size = value_size(value);

    if(src_size >= dest_size) {
        fprintf(out, "mov %s, ", dest_reg);
        emit_value_as_op(out, ctx, value);
        return;
    }

    bool stype = issigned(value.type);

    if(src_size == 4 && dest_size == 8) {
        if(stype) {
            fprintf(out, "movsxd %s, dword ", dest_reg);
            emit_value_as_op(out, ctx, value);
        } else {
            fprintf(out, "mov %s, ", dest_reg);
            emit_value_as_op(out, ctx, value);
        }

        return;
    }

    const char* size_kw = (src_size == 1) ? "byte" : "word";
    fprintf(out, "%s %s, %s ", stype ? "movsx" : "movzx", dest_reg, size_kw);
    emit_value_as_op(out, ctx, value);
}

static void emit_load(FILE *out, CodegenContext *ctx, IrValue value, RegFamily family, int size) {
    emit_load_to(out, ctx, value, ireg_name(family, size), size);
}

static void emit_fload(FILE *out, CodegenContext *ctx, IrValue value, const char* dest_reg) {
    if(value.kind == IR_VAL_CONST_FLOAT) {
        const char* label = value.type.base == KFLOAT ? "Lflt" : "Ldbl";
        fprintf(out, "mov%s %s, [rel %s%d]", float_suffix(type_bytes_size(value.type)), dest_reg, label, value.as.float_id);
        return;
    }

    fprintf(out, "mov%s %s, ", float_suffix(type_bytes_size(value.type)), dest_reg);
    emit_value_as_op(out, ctx, value);
}

static void emit_fstore(FILE *out, CodegenContext *ctx, const char* reg, IrValue dest) {
    fprintf(out, "mov%s ", float_suffix(type_bytes_size(dest.type)));
    emit_value_as_op(out, ctx, dest);
    fprintf(out, ", %s", reg);
}

static int compute_width(Typecheck type) {
    return type_bytes_size(type);
}

static void emit_store(FILE *out, CodegenContext *ctx, RegFamily family, IrValue dest) {
    int size = value_size(dest);
    fprintf(out, "mov ");
    emit_value_as_op(out, ctx, dest);
    fprintf(out, ", %s", ireg_name(family, size));
}

static void emit_label_epilogue(FILE *out, int id) {
    fprintf(out, ".Lepi%d:", id); NL;
} 

static void emit_jump_epilogue(FILE *out, int id) {
    SPACES; fprintf(out, "jmp .Lepi%d", id); NL;
}

static void emit_func_epilogue(FILE *out) {
    SPACES; fprintf(out, "leave"); NL;
    SPACES; fprintf(out, "ret"); NL;
}

static void emit_func_prologue(CodegenContext *ctx, Node *node) {
    FILE* out = ctx->out;

    SPACES; fprintf(out, "push rbp"); NL;
    SPACES; fprintf(out, "mov rbp, rsp"); NL;

    if(ctx->frame_size > 0) {
        SPACES; fprintf(out, "sub rsp, %d", ctx->frame_size); NL;
    }

    int iindex = 0, findex = 0;
    NodeList* params = node->ast.decl_function.params;
    for(size_t i = 0; i < params->count && i < 6; ++i) {
        Node* param = params->items[i];
        int offset = slot_offset(ctx, (int)i);
        int size = ctx->slot_layouts[i].size;

        if(type_isfloat(param->resolved_type)) {
            if(findex >= 6) continue;
            SPACES; fprintf(out, "mov%s [rbp%d], %s", float_suffix(size), offset, farg_name(REG_XMM0 + findex++)); NL;
        } else {
            if(iindex >= 6) continue;
            SPACES; fprintf(out, "mov [rbp%d], %s", offset, iarg_registers(width_to_family(size), iindex++)); NL;
        }
    }

    for(size_t i = 6; i < params->count; ++i) {
        int size = ctx->slot_layouts[i].size;
        int caller = 16 + (int)(i - 6) * 8;
        int offset = slot_offset(ctx, (int)i);

        if(type_isfloat(params->items[i]->resolved_type)) {
            SPACES; fprintf(out, "mov%s %s, [rbp+%d]", float_suffix(size), farg_name(REG_XMM0), caller);
            SPACES; fprintf(out, "mov%s [rbp%d], %s", float_suffix(size), offset, farg_name(REG_XMM0));
        } else {
            SPACES; fprintf(out, "mov %s, [rbp+%d]", ireg_name(REG_RAX, size), caller); NL;
            SPACES; fprintf(out, "mov [rbp%d], %s", offset, ireg_name(REG_RAX, size)); NL;
        }
    }
}

static void emit_instruction(CodegenContext *ctx, IrInstruction instruction) {
    FILE* out = ctx->out;

    switch (instruction.op)
    {
    case IR_SLOT_DECL: break;

    case IR_ARG_STACK: {
        SPACES; emit_load(out, ctx, instruction.src1, REG_RAX, 8); NL;
        SPACES; fprintf(out, "push rax"); NL;
        return;
    }

    case IR_LOAD_INDIRECT: {
        int width = compute_width(instruction.dest.type);
        SPACES; emit_load(out, ctx, instruction.src1, REG_RAX, 8); NL;
        SPACES;
        if(instruction.aux != 0) { fprintf(out, "mov %s, [rax%+d]", ireg_name(REG_RCX, width), instruction.aux); NL; }
        else { fprintf(out, "mov %s, [rax]", ireg_name(REG_RCX, width)); NL; }
        SPACES; emit_store(out, ctx, REG_RCX, instruction.dest); NL;
        return;
    }

    case IR_STORE_INDIRECT: {
        int width = compute_width(instruction.src2.type);
        SPACES; emit_load(out, ctx, instruction.src1, REG_RAX, 8); NL;
        SPACES; emit_load(out, ctx, instruction.src2, REG_RCX, width); NL;
        SPACES;
        if(instruction.aux != 0) fprintf(out, "mov [rax%+d], %s", instruction.aux, ireg_name(REG_RCX, width));
        else fprintf(out, "mov [rax], %s", ireg_name(REG_RCX, width));
        NL;
        return;
    }

    case IR_LABEL: {
        fprintf(out, ".L%d:", instruction.dest.as.label_id); NL;
        break;
    }

    case IR_JMP: {
        SPACES; fprintf(out, "jmp .L%d", instruction.dest.as.label_id); NL;
        break;
    }

    case IR_JMP_IF_ZERO: {
        int width = compute_width(instruction.src1.type);
        const char* acc = ireg_name(REG_RAX, width);
        SPACES; emit_load(out, ctx, instruction.src1, REG_RAX, width); NL;
        SPACES; fprintf(out, "test %s, %s", acc, acc); NL;
        SPACES; fprintf(out, "jz .L%d", instruction.src2.as.label_id); NL;
        return;
    }

    case IR_ASSIGN: {
        if(type_isfloat(instruction.dest.type)) {
            const char* xmm0 = freg_name(REG_XMM0);

            SPACES; emit_fload(out, ctx, instruction.src1, xmm0); NL;
            SPACES; emit_fstore(out, ctx, xmm0, instruction.dest); NL;
            return;
        }

        int width = compute_width(instruction.dest.type);
        SPACES; emit_load(out, ctx, instruction.src1, REG_RAX, width); NL;
        SPACES; emit_store(out, ctx, REG_RAX, instruction.dest); NL;
        return;
    }

    case IR_ADD: case IR_SUB: case IR_MUL: case IR_BAND: case IR_BOR: case IR_BXOR: {
        if(type_isfloat(instruction.dest.type)) {
            const char* suf = float_suffix(type_bytes_size(instruction.dest.type));
            const char* op = instruction_to_str(instruction.op);
            const char* xmm0 = freg_name(REG_XMM0);
            const char* xmm1 = freg_name(REG_XMM1);

            SPACES; emit_fload(out, ctx, instruction.src1, xmm0); NL;
            SPACES; emit_fload(out, ctx, instruction.src2, xmm1); NL;
            SPACES; fprintf(out, "%s%s %s, %s", op, suf, xmm0, xmm1); NL;
            SPACES; emit_fstore(out, ctx, xmm0, instruction.dest); NL;
            return;
        }

        int width = compute_width(instruction.dest.type);
        const char* acc = ireg_name(REG_RAX, width);
        const char* cnt = ireg_name(REG_RCX, width);
        SPACES; emit_load(out, ctx, instruction.src1, REG_RAX, width); NL;
        SPACES; emit_load(out, ctx, instruction.src2, REG_RCX, width); NL;
        SPACES; fprintf(out, "%s %s, %s", instruction_to_str(instruction.op), acc, cnt); NL;
        SPACES; emit_store(out, ctx, REG_RAX, instruction.dest); NL;
        return;
    }

    case IR_DIV: case IR_MOD: {
        if(instruction.op == IR_DIV && type_isfloat(instruction.dest.type)) {
            const char* suf = float_suffix(type_bytes_size(instruction.dest.type));
            const char* xmm0 = freg_name(REG_XMM0);
            const char* xmm1 = freg_name(REG_XMM1);

            SPACES; emit_fload(out, ctx, instruction.src1, xmm0); NL;
            SPACES; emit_fload(out, ctx, instruction.src2, xmm1); NL;
            SPACES; fprintf(out, "div%s %s, %s", suf, xmm0, xmm1); NL;
            SPACES; emit_fstore(out, ctx, xmm0, instruction.dest); NL;
            return;
        }

        int width = compute_width(instruction.dest.type);
        const char* cnt = ireg_name(REG_RCX, width);
        SPACES; emit_load(out, ctx, instruction.src1, REG_RAX, width); NL;
        SPACES; fprintf(out, "%s", width == 8 ? "cqo" : "cdq"); NL;
        SPACES; emit_load(out, ctx, instruction.src2, REG_RCX, width); NL;
        SPACES; fprintf(out, "idiv %s", cnt); NL;
        SPACES; emit_store(out, ctx, instruction.op == IR_DIV ? REG_RAX : REG_RDX, instruction.dest); NL;
        return;
    }

    case IR_SHL: case IR_SHR: {
        int width = compute_width(instruction.dest.type);
        const char* acc = ireg_name(REG_RAX, width);
        SPACES; emit_load(out, ctx, instruction.src1, REG_RAX, width); NL;
        SPACES; emit_load(out, ctx, instruction.src2, REG_RCX, 4); NL;
        SPACES; fprintf(out, "%s %s, cl", instruction.op == IR_SHL ? "shl" : "shr", acc); NL;
        SPACES; emit_store(out, ctx, REG_RAX, instruction.dest); NL;
        return;
    }

    case IR_CMP_LT: case IR_CMP_LE: case IR_CMP_GT: case IR_CMP_GE:
    case IR_CMP_EQ: case IR_CMP_NE: {
        if(type_isfloat(instruction.src1.type)) {
            const char* suf = float_suffix(type_bytes_size(instruction.src1.type));
            const char* xmm0 = freg_name(REG_XMM0);
            const char* xmm1 = freg_name(REG_XMM1);

            const char* set = { 0 };
            switch (instruction.op)
            {
            case IR_CMP_LT: set = "setb"; break;
            case IR_CMP_LE: set = "setbe"; break;
            case IR_CMP_GT: set = "seta"; break;
            case IR_CMP_GE: set = "setae"; break;
            case IR_CMP_EQ: set = "sete"; break;
            default: set = "setne"; break;
            }

            SPACES; emit_fload(out, ctx, instruction.src1, xmm0); NL;
            SPACES; emit_fload(out, ctx, instruction.src2, xmm1); NL;
            SPACES; fprintf(out, "ucomi%s %s, %s", suf, xmm0, xmm1); NL;
            SPACES; fprintf(out, "%s al", set); NL;
            SPACES; fprintf(out, "movzx eax, al"); NL;
            SPACES; emit_store(out, ctx, REG_RAX, instruction.dest); NL;
            return;
        }

        int width = compute_width(instruction.src1.type);
        const char* acc = ireg_name(REG_RAX, width);
        const char* cnt = ireg_name(REG_RCX, width);

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

        SPACES; emit_load(out, ctx, instruction.src1, REG_RAX, width); NL;
        SPACES; emit_load(out, ctx, instruction.src2, REG_RCX, width); NL;
        SPACES; fprintf(out, "cmp %s, %s", acc, cnt); NL;
        SPACES; fprintf(out, "%s al", set); NL;
        if(width > 1) {
            SPACES; fprintf(out, "movzx %s, al", acc); NL;
        }
        SPACES; emit_store(out, ctx, REG_RAX, instruction.dest); NL;
        return;
    }

    case IR_NEG: {
        if(type_isfloat(instruction.src1.type)) {
            const char* suf = float_suffix(type_bytes_size(instruction.src1.type));
            const char* xmm0 = freg_name(REG_XMM0);
            const char* xmm1 = freg_name(REG_XMM1);

            SPACES; fprintf(out, "pxor %s, %s", xmm1, xmm1); NL;
            SPACES; emit_fload(out, ctx, instruction.src1, xmm0); NL;
            SPACES; fprintf(out, "sub%s, %s, %s", suf, xmm1, xmm0); NL;
            SPACES; emit_fstore(out, ctx, xmm1, instruction.dest); NL;
            return;
        }

        int width = compute_width(instruction.src1.type);
        const char* acc = ireg_name(REG_RAX, width);
        SPACES; emit_load(out, ctx, instruction.src1, REG_RAX, width); NL;
        SPACES; fprintf(out, "neg %s", acc); NL;
        SPACES; emit_store(out, ctx, REG_RAX, instruction.dest); NL;
        return;
    }

    case IR_NOT: {
        int width = compute_width(instruction.src1.type);
        const char* acc = ireg_name(REG_RAX, width);
        SPACES; emit_load(out, ctx, instruction.src1, REG_RAX, width); NL;
        SPACES; fprintf(out, "test %s, %s", acc, acc); NL;
        SPACES; fprintf(out, "sete al"); NL;
        if(width > 1) {
            SPACES; fprintf(out, "movzx %s, al", acc); NL;
        }
        SPACES; emit_store(out, ctx, REG_RAX, instruction.dest); NL;
        return;
    }

    case IR_RETURN: {
        if(instruction.src1.kind != IR_VAL_NONE) {
            if(type_isfloat(instruction.src1.type)) {
                SPACES; emit_fload(out, ctx, instruction.src1, "xmm0"); NL;
            } else {
                int width = compute_width(instruction.src1.type);
                SPACES; emit_load(out, ctx, instruction.src1, REG_RAX, width); NL;
            }
        }
        emit_jump_epilogue(out, ctx->current_func_id);
        return;
    }

    case IR_ARG: {
        if(type_isfloat(instruction.src1.type)) {
            SPACES; emit_fload(out, ctx, instruction.src1, freg_name(instruction.aux + REG_XMM0)); NL;
            return;
        }

        int bwidth = compute_width(instruction.src1.type);
        bool builtin = ctx->is_builtin_arg[ctx->current_instruc_index];
        int width = builtin ? 8 : bwidth;

        const char* reg = iarg_registers(width_to_family(width), instruction.aux);
        SPACES; emit_load_to(out, ctx, instruction.src1, reg, width); NL;
        return;
    }

    case IR_CALL: {
        if(instruction.src1.kind == IR_VAL_BUILTIN) {
            SPACES; fprintf(out, "call __syscall_builtin"); NL;
        } else {
            View name = instruction.src1.func_name;
            SPACES; fprintf(out, "call %.*s", (int)name.len, name.start); NL;
        }

        SPACES; emit_store(out, ctx, REG_RAX, instruction.dest); NL;
        int stack_args = instruction.aux > 6 ? instruction.aux - 6 : 0;
        if(stack_args > 0) {
            SPACES; fprintf(out, "add rsp, %d", stack_args * 8); NL;
        }
        return;
    }

    case IR_ALLOCA: {
        int width = compute_width(instruction.src1.type);
        SPACES; emit_load(out, ctx, instruction.src1, REG_RAX, width); NL;
        SPACES; fprintf(out, "add rax, 15"); NL;
        SPACES; fprintf(out, "and rax, -16"); NL;
        SPACES; fprintf(out, "sub rsp, rax"); NL;
        SPACES; fprintf(out, "mov ");
        emit_value_as_op(out, ctx, instruction.dest);
        fprintf(out, ", rsp"); NL;
        return;
    }

    case IR_STACK_SAVE: {
        SPACES; fprintf(out, "mov ");
        emit_value_as_op(out, ctx, instruction.dest);
        fprintf(out, ", rsp"); NL;
        return;
    }

    case IR_STACK_RESTORE: {
        SPACES; fprintf(out, "mov rsp, ");
        emit_value_as_op(out, ctx, instruction.src1);
        NL;
        return;
    }

    case IR_ATOMIC_CAS: {
        int width = compute_width(instruction.src2.type);
        const char* cnt = ireg_name(REG_RCX, width);

        SPACES; emit_load(out, ctx, instruction.src1, REG_R11, 8); NL;
        SPACES; emit_load(out, ctx, instruction.src2, REG_RAX, width); NL;
        SPACES; emit_load(out, ctx, instruction.src3, REG_RCX, width); NL;
        SPACES; fprintf(out, "lock cmpxchg [r11], %s", cnt); NL;
        SPACES; fprintf(out, "sete al"); NL;
        SPACES; fprintf(out, "movzx eax, al"); NL;
        SPACES; emit_store(out, ctx, REG_RAX, instruction.dest); NL;
        return;
    }

    case IR_ATOMIC_ADD: {
        int width = compute_width(instruction.src1.type);
        const char* acc = ireg_name(REG_RAX, width);
        const char* cnt = ireg_name(REG_RCX, width);

        SPACES; emit_load(out, ctx, instruction.src2, REG_RCX, width); NL;
        SPACES; fprintf(out, "lock xadd ");
        emit_value_as_op(out, ctx, instruction.src1);
        fprintf(out, ", %s", cnt); NL;

        if(instruction.dest.kind == IR_VAL_NONE) return;

        SPACES; emit_load(out, ctx, instruction.src2, REG_RAX, width); NL;
        SPACES; fprintf(out, "add %s, %s", acc, cnt); NL;
        SPACES; emit_store(out, ctx, REG_RAX, instruction.dest); NL;
        return;
    }

    case IR_ATOMIC_AND: case IR_ATOMIC_XOR: case IR_ATOMIC_OR: {
        int width = compute_width(instruction.src1.type);
        const char* cnt = ireg_name(REG_RCX, width);

        SPACES; emit_load(out, ctx, instruction.src2, REG_RCX, width); NL;
        SPACES; fprintf(out, "lock %s ", instruction_to_str(instruction.op));
        emit_value_as_op(out, ctx, instruction.src1);
        fprintf(out, ", %s", cnt); NL;

        if(instruction.dest.kind == IR_VAL_NONE) return;

        SPACES; emit_load(out, ctx, instruction.src1, REG_RAX, width); NL;
        SPACES; emit_store(out, ctx, REG_RAX, instruction.dest); NL;
        return;
    }

    case IR_ATOMIC_SHL: case IR_ATOMIC_SHR: {
        int width = compute_width(instruction.src1.type);
        SPACES; emit_load(out, ctx, instruction.src2, REG_RCX, 4); NL;
        SPACES; fprintf(out, "lock %s ", instruction.op == IR_ATOMIC_SHL ? "shl" : "shr");
        emit_value_as_op(out, ctx, instruction.src1);
        fprintf(out, ", cl"); NL;

        if(instruction.dest.kind == IR_VAL_NONE) return;

        SPACES; emit_load(out, ctx, instruction.src1, REG_RAX, width); NL;
        SPACES; emit_store(out, ctx, REG_RAX, instruction.dest); NL;
        return;
    }

    case IR_ATOMIC_LOAD: {
        int width = compute_width(instruction.dest.type);
        SPACES; emit_load(out, ctx, instruction.src1, REG_RAX, 8); NL;
        SPACES; fprintf(out, "mov %s, [rax]", ireg_name(REG_RCX, width)); NL;
        SPACES; emit_store(out, ctx, REG_RCX, instruction.dest); NL;
        return;
    }

    case IR_ATOMIC_STORE: {
        int width = compute_width(instruction.src2.type);
        SPACES; emit_load(out, ctx, instruction.src1, REG_RAX, 8); NL;
        SPACES; emit_load(out, ctx, instruction.src2, REG_RCX, width); NL;
        SPACES; fprintf(out, "xchg [rax], %s", ireg_name(REG_RCX, width)); NL;
        return;
    }

    case IR_ATOMIC_XCHG: {
        int width = compute_width(instruction.src2.type);
        SPACES; emit_load(out, ctx, instruction.src1, REG_RAX, 8); NL;
        SPACES; emit_load(out, ctx, instruction.src2, REG_RCX, width); NL;
        SPACES; fprintf(out, "xchg [rax], %s", ireg_name(REG_RCX, width)); NL;
        SPACES; emit_store(out, ctx, REG_RCX, instruction.dest); NL;
        return;
    }
    default:
        return;
    }
}

static void emit_function(CodegenContext *ctx, Node *node, IrGenContext *ir_ctx, IrFunctionRange range) {
    FILE* out = ctx->out;
    ctx->current_func_id = node->func_id;

    compute_layouts(ctx, ir_ctx, range.start, range.end, range.slot_count, range.temp_count);

    View name = node->ast.decl_function.name;

    if(!node->ast.decl_function._static && !view_equals_view(name, (View){ "start", 5 })) {
        fprintf(out, "global %.*s", (int)name.len, name.start); NL;
    }

    bool _entry = (node == ctx->entry);
    if(_entry) {
        fprintf(out, "start:"); NL;
    } else {
        fprintf(out, "%.*s:", (int)name.len, name.start); NL;
    }

    emit_func_prologue(ctx, node);

    for(size_t i = range.start; i < range.end; ++i) {
        ctx->current_instruc_index = i;
        emit_instruction(ctx, ir_ctx->instructions.items[i]);
    }

    emit_label_epilogue(out, ctx->current_func_id);
    emit_func_epilogue(out);
}

static bool* build_builtins_arg_map(Arena *arena, IrInstruction *items, size_t start, size_t end) {
    bool* is_bultinarg = (bool*)arena_alloc(arena, sizeof(bool) * (end - start > 0 ? end - start : 1));
    bool pending = false;

    for(size_t i = end; i < start; --i) {
        size_t idx = i - 1;
        IrInstruction instruction = items[idx];

        if(instruction.op == IR_CALL) {
            pending = (instruction.src1.kind == IR_VAL_BUILTIN);
            is_bultinarg[idx] = false;
        } else if(instruction.op == IR_ARG) {
            is_bultinarg[idx] = pending;
        } else {
            pending = false;
            is_bultinarg[idx] = false;
        }
    }

    return is_bultinarg;
}

static void emit_extern(CodegenContext *ctx) {
    FILE* out = ctx->out;

    for(size_t i = 0; i < func_count(); ++i)
        for(FuncEntry* entry = func_item(i); entry != NULL; entry = entry->next)
            if(entry->_extern && !entry->has_body) {
                fprintf(out, "extern %.*s", (int)entry->name.len, entry->name.start); NL;
            }
}

CodegenContext create_codegen(FILE *out, Arena *arena, Node *entry, CheckContext *check_ctx) {
    CodegenContext ctx;
    ctx.out = out;
    ctx.arena = arena;
    ctx.entry = entry;
    ctx.check_ctx = check_ctx;
    ctx.current_func_id = 0;
    ctx.slot_layouts = NULL;
    ctx.temp_layouts = NULL;
    ctx.is_builtin_arg = NULL;
    ctx.current_instruc_index = 0;
    ctx.frame_size = 0;
    return ctx;
}

void emit_program(CodegenContext *ctx, IrGenContext *ir) {
    FILE* out = ctx->out;

    if(ir->strings.count > 0 || ir->floats.count > 0) {
        fprintf(out, "section .data"); NL;

        for(size_t i = 0; i < ir->strings.count; ++i) {
            IrStringEntry* entry = &ir->strings.items[i];
            SPACES; fprintf(out, "Lstr%d: db ", entry->id);
            for(size_t j = 0; j < entry->text.len; ++j) {
                unsigned char c = (unsigned char)entry->text.start[j];

                if(c == '\\') {
                    j++;

                    switch (entry->text.start[j])
                    {
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

        for(size_t i = 0; i < ir->floats.count; ++i) {
            IrFloatEntry* entry = &ir->floats.items[i];
            const char* label = entry->_single ? "Lflt" : "Ldbl";
            const char* directive = entry->_single ? "dd" : "dq";
            SPACES; fprintf(out, "%s%d: %s %.17g", label, entry->id, directive, entry->value); NL;
        }
        NL;
    }

    emit_extern(ctx);

    fprintf(out, "section .text"); NL;

    bool entry_unit = (ctx->entry != NULL);

    if(entry_unit) {
        fprintf(out, "global __syscall_builtin"); NL;
        fprintf(out, "global _start"); NL;
    } else {
        fprintf(out, "extern __syscall_builtin"); NL;
    }

    NL;

    ctx->is_builtin_arg = build_builtins_arg_map(ctx->arena, ir->instructions.items, 0, ir->instructions.count);

    for(size_t i = 0; i < ir->functions.count; ++i) {
        IrFunctionRange range = ir->functions.items[i];
        emit_function(ctx, range.func_node, ir, range);
        NL;
    }

    if(entry_unit) {
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
