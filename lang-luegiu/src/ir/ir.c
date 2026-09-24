#include "../../include/ir.h"

#define ATOMIC_OP_FENCE 1
#define ATOMIC_OP_CAS 2
#define ATOMIC_OP_LOAD 3
#define ATOMIC_OP_STORE 4
#define ATOMIC_OP_XCHG 5

static size_t primitive_size(TokenType type) {
    switch (type)
    {
    case KINT8: case KUINT8: case KCHAR: return 1;
    case KINT16: case KUINT16: return 2;
    case KINT32: case KUINT32: case KFLOAT: return 4;
    case KINT64: case KUINT64: case KHEXA: case KDOUBLE: return 8;
    default: return 8;
    }
}

static IrOperators tk_to_atomicop(TokenType op) {
    switch (op)
    {
    case OP_ANDEQ: return IR_ATOMIC_AND;
    case OP_XOREQ: return IR_ATOMIC_XOR;
    case OP_OREQ: return IR_ATOMIC_OR;
    case OP_LSHIFTEQ: return IR_ATOMIC_SHL;
    case OP_RSHIFTEQ: return IR_ATOMIC_SHR;
    default: return IR_ADD;
    }
}

static IrOperators tk_to_op(TokenType op) {
    switch (op)
    {
    case OP_PLUSEQ: return IR_ADD;
    case OP_MINUSEQ: return IR_SUB;
    case OP_LSHIFTEQ: return IR_SHL;
    case OP_RSHIFTEQ: return IR_SHR;
    case OP_ANDEQ: return IR_BAND;
    case OP_OREQ: return IR_BOR;
    case OP_XOREQ: return IR_BXOR;
    default: return IR_ADD;
    }
}

static bool decl_is_vla(Node *stmt) {
    if(stmt->kind != NODE_VAR_CALL) return false;
    return stmt->resolved_type._array && stmt->resolved_type._vla;
}

static bool block_has_vla(NodeList *list) {
    for(size_t i = 0; i < list->count; ++i) {
        if(decl_is_vla(list->items[i])) return true;
    }

    return false;
}

static bool type_isfloat(Typecheck type) {
    if(type.ptr_lvl > 0) return false;
    return type.base == KFLOAT || type.base == KDOUBLE;
}

static int intern_string(IrGenContext *ctx, View txt) {
    for(size_t i = 0; i < ctx->strings.count; ++i) {
        View exist = ctx->strings.items[i].text;
        if(exist.len == txt.len && memcmp(exist.start, txt.start, txt.len) == 0)
            return ctx->strings.items[i].id;
    }

    if(ctx->strings.count == ctx->strings.capacity) {
        size_t new_cap = (ctx->strings.capacity > 0) ? ctx->strings.capacity * 2 : 8;
        IrStringEntry* new_items = (IrStringEntry*)arena_alloc(ctx->arena, sizeof(IrStringEntry) * new_cap);
        if(ctx->strings.capacity > 0) memcpy(new_items, ctx->strings.items, sizeof(IrStringEntry) * ctx->strings.count);
        ctx->strings.capacity = new_cap;
        ctx->strings.items = new_items;
    }

    int id = (int)ctx->strings.count;
    ctx->strings.items[ctx->strings.count++] = (IrStringEntry){ txt, id };
    return id;
}

static int intern_float(IrGenContext *ctx, double val, bool issingle) {
    for(size_t i = 0; i < ctx->floats.count; ++i) {
        IrFloatEntry entry = ctx->floats.items[i];
        if(entry._single == issingle && entry.value == val) return entry.id;
    }

    if(ctx->floats.count == ctx->floats.capacity) {
        size_t new_cap = (ctx->floats.capacity > 0) ? ctx->floats.capacity * 2 : 8;
        IrFloatEntry* new_items = (IrFloatEntry*)arena_alloc(ctx->arena, sizeof(IrFloatEntry) * new_cap);
        if(ctx->floats.count > 0) memcpy(new_items, ctx->floats.items, sizeof(IrFloatEntry) * ctx->floats.count);
        ctx->floats.capacity = new_cap;
        ctx->floats.items = new_items;
    }

    int id = (int)ctx->floats.count;
    ctx->floats.items[ctx->floats.count++] = (IrFloatEntry){ val, id, issingle };
    return id;
}

static size_t sizeof_type_ir(Typecheck type) {
    if(type.ptr_lvl > 0) return 8;
    if(type._vla) return 8;
    if(type.inline_def != NULL) return type.inline_def->size;

    return primitive_size(type.base);
}

static long long ptr_step_size(Typecheck type) {
    if(type.ptr_lvl == 0) return 1;

    Typecheck ptr = type;
    ptr.ptr_lvl--;
    return (long long)sizeof_type_ir(ptr);
}

static void list_push(Arena *arena, IrInstructionList *list, IrInstruction instruction) {
    if(list->count >= list->capacity) {
        size_t new_cap = (list->capacity == 0) ? 16 : list->capacity * 2;
        IrInstruction* new_items = (IrInstruction*)arena_alloc(arena, sizeof(IrInstruction) * new_cap);
        if(list->count > 0) memcpy(new_items, list->items, sizeof(IrInstruction) * list->count);
        list->items = new_items;
        list->capacity = new_cap;
    }

    list->items[list->count++] = instruction;
}

static void emit(IrGenContext *ctx, IrOperators op, IrValue dest, IrValue src1, IrValue src2, int aux) {
    IrInstruction instruction = { 0 };
    instruction.op = op;
    instruction.dest = dest;
    instruction.src1 = src1;
    instruction.src2 = src2;
    instruction.aux = aux;
    list_push(ctx->arena, &ctx->instructions, instruction);
}

static IrValue none_value(void) {
    IrValue value = { 0 };
    value.kind = IR_VAL_NONE;
    return value;
}

static bool value_atomic(IrValue value) {
    return value.type._atomic;
}

static bool iscompoundop(TokenType op) {
    return isarith_compoundop(op) || isbitwise_compoundop(op);
}

static IrValue new_temp(IrGenContext *ctx, Typecheck type) {
    IrValue value = { 0 };
    value.kind = IR_VAL_TEMP;
    value.as.temp_id = ctx->next_temp++;
    value.type = type;
    return value;
}

static int new_label(IrGenContext *ctx) {
    return ctx->next_label++;
}

static long long const_op_value(Node *node) {
    if(node->kind == NODE_LITERAL && (node->ast.literal.kind == INT || node->ast.literal.kind == BIG))
        return node->ast.literal.integer64;
    if(node->kind == NODE_ENUM_CALL)
        return node->ast.enum_access.iresolved_type;
    return -1;
}

static IrGenScope* gen_scope_push(IrGenContext *ctx) {
    IrGenScope* scope = (IrGenScope*)arena_alloc(ctx->arena, sizeof(IrGenScope));
    memset(scope->buckets, 0, sizeof(scope->buckets));
    scope->parent = ctx->current_scope;
    ctx->current_scope = scope;
    return scope;
}

static void gen_scope_pop(IrGenContext *ctx) {
    ctx->current_scope = ctx->current_scope->parent;
}

static int scope_declare(IrGenContext *ctx, View name, Typecheck type) {
    int slot = ctx->next_slot++;

    uint64_t hash = view_hash(name)& (IR_SCOPE_TABLE_SIZE - 1);

    IrSlotEntry* entry = (IrSlotEntry*)arena_alloc(ctx->arena, sizeof(IrSlotEntry));
    entry->name = name;
    entry->slot_id = slot;
    entry->type = type;
    entry->is_array = false;
    entry->dim_count = 0;
    entry->dim_strides = NULL;
    entry->next = ctx->current_scope->buckets[hash];
    ctx->current_scope->buckets[hash] = entry;

    return slot;
}

static int gen_scope_declare_array(IrGenContext *ctx, View name, Typecheck type, size_t dim_count, IrValue *strides) {
    int slot = ctx->next_slot++;

    uint64_t hash = view_hash(name)& (IR_SCOPE_TABLE_SIZE - 1);

    IrSlotEntry* entry = (IrSlotEntry*)arena_alloc(ctx->arena, sizeof(IrSlotEntry));
    entry->name = name;
    entry->slot_id = slot;
    entry->type = type;
    entry->dim_count = dim_count;
    entry->dim_strides = strides;
    entry->is_array = true;
    entry->next = ctx->current_scope->buckets[hash];
    ctx->current_scope->buckets[hash] = entry;

    return slot;
}

static IrSlotEntry* gen_scope_lookup(IrGenContext *ctx, View name) {
    uint64_t hash = view_hash(name)& (IR_SCOPE_TABLE_SIZE - 1);

    for(IrGenScope* scope = ctx->current_scope; scope != NULL; scope = scope->parent) {
        for(IrSlotEntry* entry = scope->buckets[hash]; entry != NULL; entry = entry->next) {
            if(view_equals_view(entry->name, name)) return entry;
        }
    }

    return NULL;
}

static void ir_func_list_push(Arena *arena, IrFunctionRangeList *list, IrFunctionRange range) {
    if(list->count >= list->capacity) {
        size_t new_cap = (list->capacity == 0) ? 8 : list->capacity * 2;
        IrFunctionRange* new_items = (IrFunctionRange*)arena_alloc(arena, sizeof(IrFunctionRange) * new_cap);
        if(list->count > 0) memcpy(new_items, list->items, sizeof(IrFunctionRange) * list->count);
        list->items = new_items;
        list->capacity = new_cap;
    }

    list->items[list->count++] = range;
}

static void register_func_id(IrGenContext *ctx, View name, int func_id) {
    uint64_t hash = view_hash(name)& (IR_FUNC_TABLE_SIZE - 1);

    IrFuncIdEntry* entry = (IrFuncIdEntry*)arena_alloc(ctx->arena, sizeof(IrFuncIdEntry));
    entry->name = name;
    entry->func_id = func_id;
    entry->next = ctx->func_id_buckets[hash];
    ctx->func_id_buckets[hash] = entry;
}

static int lookup_func_id(IrGenContext *ctx, View name) {
    uint64_t hash = view_hash(name)& (IR_FUNC_TABLE_SIZE - 1);

    for(IrFuncIdEntry* entry = ctx->func_id_buckets[hash]; entry != NULL; entry = entry->next) {
        if(view_equals_view(entry->name, name)) return entry->func_id;
    }

    return -1;
}

static int lookup_or_create_label(IrGenContext *ctx, View name) {
    uint64_t hash = view_hash(name)& (IR_LABEL_TABLE_SIZE - 1);

    for(IrLabelEntry* entry = ctx->label_buckets[hash]; entry != NULL; entry = entry->next) {
        if(view_equals_view(entry->name, name)) return entry->label_id;
    }

    int id = new_label(ctx);
    IrLabelEntry* entry = (IrLabelEntry*)arena_alloc(ctx->arena, sizeof(IrLabelEntry));
    entry->name = name;
    entry->label_id = id;
    entry->next = ctx->label_buckets[hash];
    ctx->label_buckets[hash] = entry;
    return id;
}

static IrOperators binary_to_ir(TokenType op) {
    switch (op)
    {
    case OP_PLUS: return IR_ADD;
    case OP_MINUS: return IR_SUB;
    case OP_SLASH: return IR_DIV;
    case OP_MOD: return IR_MOD;
    case OP_STAR: return IR_MUL;
    case OP_AND: return IR_BAND;
    case OP_XOR: return IR_BXOR;
    case OP_OR: return IR_BOR;
    case OP_LT: return IR_CMP_LT;
    case OP_LE: return IR_CMP_LE;
    case OP_GT: return IR_CMP_GT;
    case OP_GE: return IR_CMP_GE;
    case OP_LSHIFT: return IR_SHL;
    case OP_RSHIFT: return IR_SHR;
    case OP_EQUALS: return IR_CMP_EQ;
    case OP_BANGEQ: return IR_CMP_NE;
    default: return IR_ADD;
    }
}

static IrValue gen_expr(IrGenContext *ctx, Node *node);

static IrValue gen_field_access(IrGenContext *ctx, Node *node) {
    int offset = (int)node->ast.field_access.offset;

    if(node->ast.field_access.arrow) {
        IrValue ptr = gen_expr(ctx, node->ast.field_access.base);
        IrValue dest = new_temp(ctx, node->resolved_type);

        IrInstruction instruction = { 0 };
        instruction.op = IR_LOAD_INDIRECT;
        instruction.dest = dest;
        instruction.src1 = ptr;
        instruction.aux = offset;
        list_push(ctx->arena, &ctx->instructions, instruction);
        return dest;
    }

    IrValue value = gen_expr(ctx, node->ast.field_access.base);
    value.field_offset += offset;
    value.type = node->resolved_type;
    return value;
}

static IrValue gen_bytes(Node *node) {
    IrValue value = {0};

    value.kind = IR_VAL_CONST_UINT;
    value.as.const_ui = node->ast.bytes.size;
    value.type = node->resolved_type;

    return value;
}

static IrValue gen_literal(IrGenContext *ctx, Node *node) {
    IrValue value = { 0 };
    value.type = node->resolved_type;

    switch (node->ast.literal.kind)
    {
    case UNSIGNED:
    case HEXA:
        value.kind = IR_VAL_CONST_UINT;
        value.as.const_ui = node->ast.literal.unsigned64;
        break;
    case INT:
    case BIG:
        value.kind = IR_VAL_CONST_INT;
        value.as.const_i = node->ast.literal.integer64;
        break;
    case DOUBLE:
    case FLOAT:
        value.kind = IR_VAL_CONST_FLOAT;
        bool single = (node->resolved_type.base == KFLOAT);
        value.as.float_id = intern_float(ctx, node->ast.literal.double64, single);
        break;
    case CHAR: {
        unsigned char c = (unsigned char)node->ast.literal.character.start[0];
        value.kind = IR_VAL_CONST_UINT;

        if(c == '\\') {
            unsigned char next = (unsigned char)node->ast.literal.character.start[1];

            switch (next)
            {
            case 'n': c = 10; break;
            case 't': c = 9; break;
            case 'r': c = 13; break;
            case '\\': c = 92; break;
            case '0': c = 0; break;
            }
        }

        value.as.const_i = (long long)c;
        break;
    }
    case STRING: {
        value.kind = IR_VAL_CONST_STRING;
        value.as.string_id = intern_string(ctx, node->ast.literal.string);
        break;
    }

    default:
        value.kind = IR_VAL_CONST_INT;
        value.as.const_i = 0;
        break;
    }

    return value;
}

static IrValue gen_atomic_compound(IrGenContext *ctx, TokenType op, IrValue address, IrValue operand, Typecheck type, int offset) {
    IrValue dest = new_temp(ctx, type);

    if(isarith_compoundop(op)) {
        if(op == OP_MINUSEQ) {
            IrValue negate = new_temp(ctx, operand.type);
            emit(ctx, IR_NEG, negate, operand, none_value(), 0);
            operand = negate;
        }

        emit(ctx, IR_ATOMIC_ADD, dest, address, operand, offset);
        return dest;
    }

    IrOperators atomic_op = tk_to_atomicop(op);
    emit(ctx, atomic_op, dest, address, operand, offset);
    return dest;
}

static IrValue gen_array_address(IrGenContext *ctx, Node *node) {
    size_t depth = 0;
    for(Node* probe = node; probe->kind == NODE_ARRAY; probe = probe->ast.binary.left) {
        depth++;
    }

    Node** indexes = (Node**)arena_alloc(ctx->arena, sizeof(Node*) * (depth > 0 ? depth : 1));

    Node* current = node;
    for(size_t i = 0; i < depth; ++i) {
        indexes[i] = current->ast.binary.right;
        current = current->ast.binary.left;
    }

    Typecheck type = { 0 };
    type.ptr_lvl = 1;

    IrValue address = { 0 };

    if(current->kind == NODE_VAR_CALL) {
        IrSlotEntry* entry = gen_scope_lookup(ctx, current->ast.call_variable.name);
        address.kind = IR_VAL_SLOT;
        address.as.slot_id = entry ? entry->slot_id : -1;
        address.type = type;
        address.field_offset = 0;

        for(size_t dimention = 0; dimention < depth; ++dimention) {
            IrValue index_val = gen_expr(ctx, indexes[depth - 1 - dimention]);
            IrValue stride = { 0 };

            if(entry && entry->is_array && dimention < entry->dim_count) {
                stride = entry->dim_strides[dimention];
            } else {
                Typecheck ptr = entry ? entry->type : current->resolved_type;
                if(ptr.ptr_lvl > 0) ptr.ptr_lvl--;

                IrValue elem_size = { 0 };
                elem_size.kind = IR_VAL_CONST_INT;
                elem_size.as.const_i = (long long)sizeof_type_ir(ptr);
                stride = elem_size;
            }

            IrValue offset = new_temp(ctx, type);
            emit(ctx, IR_MUL, offset, index_val, stride, 0);

            IrValue new_address = new_temp(ctx, type);
            emit(ctx, IR_ADD, new_address, address, offset, 0);
            address = new_address;
        }
    } else {
        address = gen_expr(ctx, current);
        for(size_t dimention = 0; dimention < depth; ++dimention) {
            IrValue index_val = gen_expr(ctx, indexes[depth - 1 - dimention]);

            IrValue elem_size = { 0 };
            elem_size.kind = IR_VAL_CONST_INT;
            elem_size.as.const_i = (long long)sizeof_type_ir(node->resolved_type);

            IrValue offset = new_temp(ctx, type);
            emit(ctx, IR_MUL, offset, index_val, elem_size, 0);

            IrValue new_address = new_temp(ctx, type);
            emit(ctx, IR_ADD, new_address, address, offset, 0);
            address = new_address;
        }
    }

    return address;
}

static IrValue gen_binary_op(IrGenContext *ctx, Node *node) {
    TokenType op = node->ast.binary.op;
    Node* left = node->ast.binary.left;
    Node* right = node->ast.binary.right;

    if(op == OP_ASSIGN) {
        if(left->kind == NODE_FIELD_CALL && left->ast.field_access.arrow) {
            IrValue ptr = gen_expr(ctx, left->ast.field_access.base);
            IrValue src = gen_expr(ctx, right);

            IrInstruction instruction = { 0 };
            instruction.op = IR_STORE_INDIRECT;
            instruction.src1 = ptr;
            instruction.src2 = src;
            instruction.aux = (int)left->ast.field_access.offset;
            list_push(ctx->arena, &ctx->instructions, instruction);
            return src;
        }

        if(left->kind == NODE_ARRAY) {
            IrValue address = gen_array_address(ctx, left);
            IrValue src = gen_expr(ctx, right);

            IrInstruction instruction = { 0 };
            instruction.op = IR_STORE_INDIRECT;
            instruction.src1 = address;
            instruction.src2 = src;
            instruction.aux = 0;
            list_push(ctx->arena, &ctx->instructions, instruction);
            return src;
        }

        IrValue dest = gen_expr(ctx, left);
        IrValue src = gen_expr(ctx, right);
        emit(ctx, IR_ASSIGN, dest, src, none_value(), 0);
        return dest;
    }

    if(op == OP_MINUS && left->resolved_type.ptr_lvl > 0 && right->resolved_type.ptr_lvl > 0) {
        IrValue lval = gen_expr(ctx, left);
        IrValue rval = gen_expr(ctx, right);

        IrValue raw = new_temp(ctx, node->resolved_type);
        emit(ctx, IR_SUB, raw, lval, rval, 0);

        IrValue step = { 0 };
        step.kind = IR_VAL_CONST_INT;
        step.as.const_i = ptr_step_size(left->resolved_type);

        IrValue dest = new_temp(ctx, node->resolved_type);
        emit(ctx, IR_DIV, dest, raw, step, 0);
        return dest;
    }

    if((op == OP_PLUS || op == OP_MINUS) && 
        (left->resolved_type.ptr_lvl > 0) != (right->resolved_type.ptr_lvl > 0)) {

        Node* pnode = left->resolved_type.ptr_lvl > 0 ? left : right;
        Node* inode = pnode == left ? right : left;

        IrValue pval = gen_expr(ctx, pnode);
        IrValue ival = gen_expr(ctx, inode);

        IrValue step = { 0 };
        step.kind = IR_VAL_CONST_INT;
        step.as.const_i = ptr_step_size(pnode->resolved_type);

        IrValue scale = new_temp(ctx, inode->resolved_type);
        emit(ctx, IR_MUL, scale, ival, step, 0);

        IrValue dest = new_temp(ctx, node->resolved_type);
        emit(ctx, op == OP_PLUS ? IR_ADD : IR_SUB, dest, pval, scale, 0);
        return dest;
    }

    if(iscompoundop(op)) {
        if(left->kind == NODE_VAR_CALL || (left->kind == NODE_FIELD_CALL && !left->ast.field_access.arrow)) {
            IrValue slot = gen_expr(ctx, left);
            IrValue operand = gen_expr(ctx, right);

            if(slot.type.ptr_lvl > 0 && (op == OP_PLUSEQ || op == OP_MINUSEQ)) {
                IrValue step = { 0 };
                step.kind = IR_VAL_CONST_INT;
                step.as.const_i = ptr_step_size(slot.type);

                IrValue scale = new_temp(ctx, operand.type);
                emit(ctx, IR_MUL, scale, operand, step, 0);
                operand = scale;
            }

            if(value_atomic(slot)) return gen_atomic_compound(ctx, op, slot, operand, slot.type, 0);

            emit(ctx, tk_to_op(op), slot, slot, operand, 0);
            return slot;
        }

        if(left->kind == NODE_ARRAY) {
            IrValue address = gen_array_address(ctx, left);
            IrValue operand = gen_expr(ctx, node->ast.binary.right);

            if(node->resolved_type.ptr_lvl > 0 && (op == OP_PLUSEQ || op == OP_MINUSEQ)) {
                IrValue step = { 0 };
                step.kind = IR_VAL_CONST_INT;
                step.as.const_i = ptr_step_size(node->resolved_type);

                IrValue scaled = new_temp(ctx, operand.type);
                emit(ctx, IR_MUL, scaled, operand, step, 0);
                operand = scaled;
            }

            if(node->resolved_type._atomic) return gen_atomic_compound(ctx, op, address, operand, node->resolved_type, 0);

            IrValue current = new_temp(ctx, node->resolved_type);
            emit(ctx, IR_LOAD_INDIRECT, current, address, none_value(), 0);

            IrValue result = new_temp(ctx, node->resolved_type);
            emit(ctx, tk_to_op(op), result, current, operand, 0);

            IrInstruction store = { 0 };
            store.op = IR_STORE_INDIRECT;
            store.src1 = address;
            store.src2 = result;
            store.aux = 0;
            list_push(ctx->arena, &ctx->instructions, store);
            return result;
        }

        if(left->kind == NODE_FIELD_CALL && left->ast.field_access.arrow) {
            IrValue ptr = gen_expr(ctx, left->ast.field_access.base);
            int offset = (int)left->ast.field_access.offset;
            IrValue operand = gen_expr(ctx, node->ast.binary.right);

            if(node->resolved_type.ptr_lvl > 0 && (op == OP_PLUSEQ || op == OP_MINUSEQ)) {
                IrValue step = { 0 };
                step.kind = IR_VAL_CONST_INT;
                step.as.const_i = ptr_step_size(node->resolved_type);

                IrValue scaled = new_temp(ctx, operand.type);
                emit(ctx, IR_MUL, scaled, operand, step, 0);
                operand = scaled;
            }

            if(node->resolved_type._atomic) return gen_atomic_compound(ctx, op, ptr, operand, node->resolved_type, offset);

            IrValue current = new_temp(ctx, node->resolved_type);

            IrInstruction load = { 0 };
            load.op = IR_LOAD_INDIRECT;
            load.dest = current;
            load.src1 = ptr;
            load.aux = offset;
            list_push(ctx->arena, &ctx->instructions, load);

            IrValue result = new_temp(ctx, node->resolved_type);
            emit(ctx, tk_to_op(op), result, current, operand, 0);

            IrInstruction store = { 0 };
            store.op = IR_STORE_INDIRECT;
            store.src1 = ptr;
            store.src2 = result;
            store.aux = offset;
            list_push(ctx->arena, &ctx->instructions, store);
            return result;
        }
    }

    IrValue lval = gen_expr(ctx, left);
    IrValue rval = gen_expr(ctx, node->ast.binary.right);

    IrValue dest = new_temp(ctx, node->resolved_type);
    emit(ctx, binary_to_ir(op), dest, lval, rval, 0);
    return dest;
}

static IrValue gen_incdec(IrGenContext *ctx, Node *operand_node, long long delta) {
    long long step = ptr_step_size(operand_node->resolved_type);

    IrValue inc = { 0 };
    inc.kind = IR_VAL_CONST_INT;
    inc.as.const_i = delta * step;

    if(operand_node->kind == NODE_VAR_CALL ||
        (operand_node->kind == NODE_FIELD_CALL && !operand_node->ast.field_access.arrow)) {

        IrValue slot = gen_expr(ctx, operand_node);
        inc.type = slot.type;

        if(value_atomic(slot)) {
            IrValue dest = new_temp(ctx, slot.type);
            emit(ctx, IR_ATOMIC_ADD, dest, slot, inc, 0);
            return dest;
        }

        emit(ctx, IR_ADD, slot, slot, inc, 0);
        return slot;
    }

    if(operand_node->kind == NODE_ARRAY) {
        IrValue address = gen_array_address(ctx, operand_node);
        inc.type = operand_node->resolved_type;

        IrValue current = new_temp(ctx, operand_node->resolved_type);
        emit(ctx, IR_LOAD_INDIRECT, current, address, none_value(), 0);

        IrValue result = new_temp(ctx, operand_node->resolved_type);
        emit(ctx, IR_ADD, result, current, inc, 0);

        IrInstruction store = { 0 };
        store.op = IR_STORE_INDIRECT;
        store.src1 = address;
        store.src2 = result;
        list_push(ctx->arena, &ctx->instructions, store);
        return result;
    }

    /* Caso p->campo++ */
    if(operand_node->kind == NODE_FIELD_CALL && operand_node->ast.field_access.arrow) {
        IrValue ptr_value = gen_expr(ctx, operand_node->ast.field_access.base);
        int offset = (int)operand_node->ast.field_access.offset;
        inc.type = operand_node->resolved_type;

        IrValue current = new_temp(ctx, operand_node->resolved_type);
        IrInstruction load = { 0 };
        load.op = IR_LOAD_INDIRECT;
        load.dest = current;
        load.src1 = ptr_value;
        load.aux = offset;
        list_push(ctx->arena, &ctx->instructions, load);

        IrValue result = new_temp(ctx, operand_node->resolved_type);
        emit(ctx, IR_ADD, result, current, inc, 0);

        IrInstruction store = { 0 };
        store.op = IR_STORE_INDIRECT;
        store.src1 = ptr_value;
        store.src2 = result;
        store.aux = offset;
        list_push(ctx->arena, &ctx->instructions, store);
        return result;
    }

    return gen_expr(ctx, operand_node);
}

static IrValue gen_unary_op(IrGenContext *ctx, Node *node) {
    IrValue operand = gen_expr(ctx, node->ast.unary.operand);

    switch (node->ast.unary.op)
    {
    case OP_MINUS: {
        IrValue dest = new_temp(ctx, node->resolved_type);
        emit(ctx, IR_NEG, dest, operand, none_value(), 0);
        return dest;
    }

    case OP_BANG: case OP_DESC: {
        IrValue dest = new_temp(ctx, node->resolved_type);
        emit(ctx, IR_NOT, dest, operand, none_value(), 0);
        return dest;
    }

    case OP_PLUS_PLUS: return gen_incdec(ctx, node->ast.unary.operand, 1);

    case OP_MINUS_MINUS: return gen_incdec(ctx, node->ast.unary.operand, -1);

    default: return operand;
    }
}

static IrValue gen_atomic_call(IrGenContext *ctx, Node *node) {
    NodeList* args = node->ast.call_function.args;

    IrValue dummy = { 0 };
    dummy.kind = IR_VAL_CONST_INT;

    if(args->count < 1) return dummy;

    long long op = const_op_value(args->items[0]);

    switch (op)
    {
    case ATOMIC_OP_FENCE: {
        emit(ctx, IR_FENCE, none_value(), none_value(), none_value(), 0);
        return dummy;
    }

    case ATOMIC_OP_CAS: {
        if(args->count < 4) return dummy;

        IrValue ptr = gen_expr(ctx, args->items[1]);
        IrValue expected = gen_expr(ctx, args->items[2]);
        IrValue desired = gen_expr(ctx, args->items[3]);
        IrValue dest = new_temp(ctx, node->resolved_type);

        IrInstruction instruction = { 0 };
        instruction.op = IR_ATOMIC_CAS;
        instruction.dest = dest;
        instruction.src1 = ptr;
        instruction.src2 = expected;
        instruction.src3 = desired;
        list_push(ctx->arena, &ctx->instructions, instruction);
        return dest;
    }

    case ATOMIC_OP_LOAD: {
        if(args->count < 2) return dummy;

        IrValue ptr = gen_expr(ctx, args->items[1]);
        IrValue dest = new_temp(ctx, node->resolved_type);

        emit(ctx, IR_ATOMIC_LOAD, dest, ptr, none_value(), 0);
        return dest;
    }

    case ATOMIC_OP_STORE: {
        if(args->count < 3) return dummy;

        IrValue ptr = gen_expr(ctx, args->items[1]);
        IrValue value = gen_expr(ctx, args->items[2]);

        emit(ctx, IR_ATOMIC_STORE, none_value(), ptr, value, 0);
        return dummy;
    }

    case ATOMIC_OP_XCHG: {
        if(args->count < 3) return dummy;

        IrValue ptr = gen_expr(ctx, args->items[1]);
        IrValue new_val = gen_expr(ctx, args->items[2]);
        IrValue dest = new_temp(ctx, node->resolved_type);

        emit(ctx, IR_ATOMIC_XCHG, dest, ptr, new_val, 0);
        return dest;
    }

    default:
        return dummy;
    }
}

static IrValue gen_func_call(IrGenContext *ctx, Node *node) {
    View name = node->ast.call_function.name;

    if(view_equals_view(name, (View){ "__atomic", 8 })) return gen_atomic_call(ctx, node);

    int func_id = lookup_func_id(ctx, name);

    NodeList* args = node->ast.call_function.args;
    size_t count = args->count > 0 ? args->count : 1;

    IrValue* args_values = (IrValue*)arena_alloc(ctx->arena, sizeof(IrValue) * count);
    int* reg_index = (int*)arena_alloc(ctx->arena, sizeof(int) * count);

    for(size_t i = 0; i < args->count; ++i) {
        args_values[i] = gen_expr(ctx, args->items[i]);
    }

    int iindex = 0, findex = 0, stack_count = 0;
    for(size_t i = 0; i < args->count; ++i) {
        if(type_isfloat(args_values[i].type)) {
            reg_index[i] = (findex < 6) ? findex++ : -1;
        } else {
            reg_index[i] = (iindex < 6) ? iindex++ : -1;
        }
        if(reg_index[i] == -1) stack_count++;
    }

    for(size_t idx = args->count; idx > 0; --idx) {
        size_t i = idx - 1;
        if(reg_index[i] == -1) {
            IrInstruction instruction = { 0 };
            instruction.op = IR_ARG_STACK;
            instruction.src1 = args_values[i];
            list_push(ctx->arena, &ctx->instructions, instruction);
        }
    }

    for(size_t i = 0; i < args->count; ++i) {
        if(reg_index[i] != -1) {
            IrInstruction instruction = { 0 };
            instruction.op = IR_ARG;
            instruction.src1 = args_values[i];
            instruction.aux = reg_index[i];
            list_push(ctx->arena, &ctx->instructions, instruction);
        }
    }

    IrValue func_ref = { 0 };
    func_ref.kind = IR_VAL_FUNC;
    if(view_equals_view(name, (View){ "__syscall_builtin", 17 })) func_ref.kind = IR_VAL_BUILTIN;
    func_ref.as.func_id = func_id;
    func_ref.func_name = name;

    IrValue dest = new_temp(ctx, node->resolved_type);

    IrInstruction call = { 0 };
    call.op = IR_CALL;
    call.dest = dest;
    call.src1 = func_ref;
    call.aux = stack_count;
    list_push(ctx->arena, &ctx->instructions, call);
    return dest;
}

static IrValue gen_enum_access(Node *node) {
    IrValue value = { 0 };
    value.kind = IR_VAL_CONST_INT;
    value.as.const_i = node->ast.enum_access.iresolved_type;
    value.type = node->resolved_type;
    return value;
}

static IrValue gen_array_index(IrGenContext *ctx, Node *node) {
    IrValue address = gen_array_address(ctx, node);
    IrValue dest = new_temp(ctx, node->resolved_type);
    emit(ctx, IR_LOAD_INDIRECT, dest, address, none_value(), 0);
    return dest;
}

static IrValue gen_var_call(IrGenContext *ctx, Node *node) {
    IrSlotEntry* entry = gen_scope_lookup(ctx, node->ast.call_variable.name);

    IrValue value = { 0 };
    value.kind = IR_VAL_SLOT;
    value.as.slot_id = entry ? entry->slot_id : -1;

    if(entry && entry->is_array) {
        Typecheck type = { 0 };
        type.ptr_lvl = 1;
        value.type = type;
    } else {
        value.type = node->resolved_type;
    }

    return value;
}

static IrValue gen_expr(IrGenContext *ctx, Node *node) {
    switch (node->kind)
    {
    case NODE_BYTES: return gen_bytes(node);
    case NODE_LITERAL: return gen_literal(ctx, node);
    case NODE_VAR_CALL: return gen_var_call(ctx, node);
    case NODE_BINARY_OP: return gen_binary_op(ctx, node);
    case NODE_POSTFIX_OP:
    case NODE_UNARY_OP: return gen_unary_op(ctx, node);
    case NODE_FUNC_CALL: return gen_func_call(ctx, node);
    case NODE_ENUM_CALL: return gen_enum_access(node);
    case NODE_FIELD_CALL: return gen_field_access(ctx, node);
    case NODE_ARRAY: return gen_array_index(ctx, node);
    default: break;
    }

    IrValue value = { 0 };
    value.kind = IR_VAL_CONST_INT;
    return value;
}

static void gen_statement(IrGenContext *ctx, Node *node);

static void gen_call_stmt(IrGenContext *ctx, Node *node) {
    if(node->ast.call_stmt.value) {
        IrValue value = gen_expr(ctx, node->ast.call_stmt.value);
        emit(ctx, IR_RETURN, none_value(), value, none_value(), 0);
    } else {
        emit(ctx, IR_RETURN, none_value(), none_value(), none_value(), 0);
    }
}

static void gen_array_decl(IrGenContext *ctx, Node *node) {
    TypeSpec* spec = &node->ast.decl_variable.call_type;
    size_t n = spec->array_dimentions->count;

    IrValue* dvalues = (IrValue*)arena_alloc(ctx->arena, sizeof(IrValue) * n);
    for(size_t i = 0; i < n; ++i) {
        dvalues[i] = gen_expr(ctx, spec->array_dimentions->items[i]);
    }

    Typecheck etype = node->resolved_type;
    etype._array = false;
    etype._vla = false;

    IrValue esize = { 0 };
    esize.kind = IR_VAL_CONST_INT;
    esize.as.const_i = sizeof_type_ir(etype);

    Typecheck stype = { 0 };
    stype.ptr_lvl = 1;

    IrValue* strides = (IrValue*)arena_alloc(ctx->arena, sizeof(IrValue) * n);
    strides[n - 1] = esize;

    for(size_t i = n - 1; i-- > 0; ) {
        IrValue stride = new_temp(ctx, stype);
        emit(ctx, IR_MUL, stride, strides[i + 1], dvalues[i + 1], 0);
        strides[i] = stride;
    }

    IrValue total_bytes = new_temp(ctx, stype);
    emit(ctx, IR_MUL, total_bytes, strides[0], dvalues[0], 0);

    int slot = gen_scope_declare_array(ctx, node->ast.decl_variable.name, node->resolved_type, n, strides);

    Typecheck ptr = { 0 };
    ptr.ptr_lvl = 1;

    IrValue pdest = { 0 };
    pdest.kind = IR_VAL_SLOT;
    pdest.as.slot_id = slot;
    pdest.type = ptr;

    emit(ctx, IR_SLOT_DECL, pdest, none_value(), none_value(), 0);
    emit(ctx, IR_ALLOCA, pdest, total_bytes, none_value(), 0);
}

static void gen_var_decl(IrGenContext *ctx, Node *node) {
    if(node->ast.decl_variable.call_type.is_array) {
        gen_array_decl(ctx, node);
        return;
    }

    int slot = scope_declare(ctx, node->ast.decl_variable.name, node->resolved_type);

    if(node->resolved_type.inline_def != NULL) {
        IrValue slot_value = { 0 };
        slot_value.kind = IR_VAL_SLOT;
        slot_value.as.slot_id = slot;
        slot_value.type = node->resolved_type;
        emit(ctx, IR_SLOT_DECL, slot_value, none_value(), none_value(), 0);
    }

    if(node->ast.decl_variable.init) {
        IrValue value = gen_expr(ctx, node->ast.decl_variable.init);
        IrValue dest = { 0 };
        dest.kind = IR_VAL_SLOT;
        dest.as.slot_id = slot;
        dest.type = node->resolved_type;
        emit(ctx, IR_ASSIGN, dest, value, none_value(), 0);
    }
}

static void gen_if_stmt(IrGenContext *ctx, Node *node) {
    IrValue cond = gen_expr(ctx, node->ast.if_stmt.condition);

    int label_else = new_label(ctx);
    int label_end = new_label(ctx);

    IrValue label_else_val = { 0 };
    label_else_val.kind = IR_VAL_LABEL;
    label_else_val.as.label_id = label_else;
    emit(ctx, IR_JMP_IF_ZERO, none_value(), cond, label_else_val, 0);

    gen_statement(ctx, node->ast.if_stmt.then);

    IrValue label_end_val = { 0 };
    label_end_val.kind = IR_VAL_LABEL;
    label_end_val.as.label_id = label_end;
    emit(ctx, IR_JMP, label_end_val, none_value(), none_value(), 0);

    IrValue label_else_define = { 0 };
    label_else_define.kind = IR_VAL_LABEL;
    label_else_define.as.label_id = label_else;
    emit(ctx, IR_LABEL, label_else_define, none_value(), none_value(), 0);

    if(node->ast.if_stmt.otherwise) gen_statement(ctx, node->ast.if_stmt.otherwise);

    IrValue label_end_define = { 0 };
    label_end_define.kind = IR_VAL_LABEL;
    label_end_define.as.label_id = label_end;
    emit(ctx, IR_LABEL, label_end_define, none_value(), none_value(), 0);
}

static void gen_block(IrGenContext *ctx, Node *node) {
    gen_scope_push(ctx);

    NodeList* statements = node->ast.program.statements;
    bool has_vla = block_has_vla(statements);

    IrValue saved = { 0 };
    if(has_vla) {
        Typecheck ptype = { 0 };
        ptype.ptr_lvl = 1;
        saved = new_temp(ctx, ptype);
        emit(ctx, IR_STACK_SAVE, saved, none_value(), none_value(), 0);
    }

    for(size_t i = 0; i < statements->count; ++i) {
        gen_statement(ctx, statements->items[i]);
    }

    if(has_vla) {
        emit(ctx, IR_STACK_RESTORE, none_value(), saved, none_value(), 0);
    }

    gen_scope_pop(ctx);
}

static void gen_func_decl(IrGenContext *ctx, Node *node) {
    bool has_body = (node->ast.decl_function.body != NULL);
    int exist_id = lookup_func_id(ctx, node->ast.decl_function.name);

    if(!has_body) {
        if(exist_id < 0) {
            node->func_id = ctx->next_func_id++;
            register_func_id(ctx, node->ast.decl_function.name, node->func_id);
        }

        return;
    }

    node->func_id = (exist_id >= 0) ? exist_id : ctx->next_func_id++;
    if(exist_id < 0) register_func_id(ctx, node->ast.decl_function.name, node->func_id);

    int sslot = ctx->next_slot;
    int stemp = ctx->next_temp;
    ctx->next_slot = 0;
    ctx->next_temp = 0;

    size_t start = ctx->instructions.count;

    gen_scope_push(ctx);

    NodeList* params = node->ast.decl_function.params;

    for(size_t i = 0; i < params->count; ++i) {
        Node* param = params->items[i];
        int slot = scope_declare(ctx, param->ast.decl_variable.name, param->resolved_type);

        IrValue dest = { 0 };
        dest.kind = IR_VAL_SLOT;
        dest.as.slot_id = slot;
        dest.type = param->resolved_type;
        emit(ctx, IR_SLOT_DECL, dest, none_value(), none_value(), 0);
    }

    if(node->ast.decl_function.body && node->ast.decl_function.body->kind == NODE_BLOCK_STMT) {
        NodeList* statements = node->ast.decl_function.body->ast.program.statements;
        for(size_t i = 0; i < statements->count; ++i) {
            gen_statement(ctx, statements->items[i]);
        }
    }

    gen_scope_pop(ctx);

    size_t end = ctx->instructions.count;

    IrFunctionRange range = { 0 };
    range.func_node = node;
    range.end = end;
    range.start = start;
    range.slot_count = ctx->next_slot;
    range.temp_count = ctx->next_temp;
    ir_func_list_push(ctx->arena, &ctx->functions, range);

    ctx->next_slot = sslot;
    ctx->next_temp = stemp;
}

static void gen_for_loop(IrGenContext *ctx, Node *node) {
    gen_scope_push(ctx);

    if(node->ast.for_loop.init) gen_statement(ctx, node->ast.for_loop.init);

    int label_start = new_label(ctx);
    int label_continue = new_label(ctx);
    int label_end = new_label(ctx);

    IrValue label_start_define = { 0 };
    label_start_define.kind = IR_VAL_LABEL;
    label_start_define.as.label_id = label_start;
    emit(ctx, IR_LABEL, label_start_define, none_value(), none_value(), 0);

    if(node->ast.for_loop.condition) {
        IrValue cond = gen_expr(ctx, node->ast.for_loop.condition);

        IrValue label_end_value = { 0 };
        label_end_value.kind = IR_VAL_LABEL;
        label_end_value.as.label_id = label_end;
        emit(ctx, IR_JMP_IF_ZERO, none_value(), cond, label_end_value, 0);
    }

    int prev_start = ctx->loop_start_label;
    int prev_end = ctx->loop_end_label;
    bool prev_in_loop = ctx->in_loop;
    ctx->loop_start_label = label_continue;
    ctx->loop_end_label = label_end;
    ctx->in_loop = true;

    gen_statement(ctx, node->ast.for_loop.body);

    ctx->loop_start_label = prev_start;
    ctx->loop_end_label = prev_end;
    ctx->in_loop = prev_in_loop;

    IrValue label_continue_def = { 0 };
    label_continue_def.kind = IR_VAL_LABEL;
    label_continue_def.as.label_id = label_continue;
    emit(ctx, IR_LABEL, label_continue_def, none_value(), none_value(), 0);

    if(node->ast.for_loop.increment) gen_expr(ctx, node->ast.for_loop.increment);

    IrValue label_start_val = { 0 };
    label_start_define.kind = IR_VAL_LABEL;
    label_start_define.as.label_id = label_start;
    emit(ctx, IR_JMP, none_value(), label_start_val, none_value(), 0);

    IrValue label_end_define = { 0 };
    label_end_define.kind = IR_VAL_LABEL;
    label_end_define.as.label_id = label_end;
    emit(ctx, IR_LABEL, label_end_define, none_value(), none_value(), 0);

    gen_scope_pop(ctx);
}

static void gen_while_loop(IrGenContext *ctx, Node *node) {
    int label_start = new_label(ctx);
    int label_end = new_label(ctx);

    IrValue label_start_define = { 0 };
    label_start_define.kind = IR_VAL_LABEL;
    label_start_define.as.label_id = label_start;
    emit(ctx, IR_LABEL, label_start_define, none_value(), none_value(), 0);

    IrValue cond = gen_expr(ctx, node->ast.while_loop.condition);

    IrValue label_end_value = { 0 };
    label_end_value.kind = IR_VAL_LABEL;
    label_end_value.as.label_id = label_end;
    emit(ctx, IR_JMP_IF_ZERO, none_value(), cond, label_end_value, 0);

    int prev_start = ctx->loop_start_label;
    int prev_end = ctx->loop_end_label;
    bool prev_loop = ctx->in_loop;
    ctx->loop_end_label = label_end;
    ctx->loop_start_label = label_start;
    ctx->in_loop = true;

    gen_statement(ctx, node->ast.while_loop.body);

    ctx->loop_start_label = prev_start;
    ctx->loop_end_label = prev_end;
    ctx->in_loop = prev_loop;

    IrValue label_start_val = { 0 };
    label_start_val.kind = IR_VAL_LABEL;
    label_start_val.as.label_id = label_start;
    emit(ctx, IR_JMP, none_value(), label_start_val, none_value(), 0);

    IrValue label_end_define = { 0 };
    label_end_define.kind = IR_VAL_LABEL;
    label_end_define.as.label_id = label_end;
    emit(ctx, IR_LABEL, label_end_define, none_value(), none_value(), 0);
}

static void gen_break_stmt(IrGenContext *ctx) {
    IrValue label_val = { 0 };
    label_val.kind = IR_VAL_LABEL;
    label_val.as.label_id = ctx->loop_end_label;
    emit(ctx, IR_JMP, none_value(), label_val, none_value(), 0);
}

static void gen_continue_stmt(IrGenContext *ctx) {
    IrValue label_val = { 0 };
    label_val.kind = IR_VAL_LABEL;
    label_val.as.label_id = ctx->loop_start_label;
    emit(ctx, IR_JMP, none_value(), label_val, none_value(), 0);
}

static void gen_statement(IrGenContext *ctx, Node *node) {
    if(!node) return;

    switch (node->kind)
    {
    case NODE_VAR_DECL: gen_var_decl(ctx, node); break;
    case NODE_IF_STMT: gen_if_stmt(ctx, node); break;
    case NODE_BLOCK_STMT: gen_block(ctx, node); break;
    case NODE_BINARY_OP:
    case NODE_POSTFIX_OP:
    case NODE_UNARY_OP: gen_expr(ctx, node); break;
    case NODE_FUNC_DECL: gen_func_decl(ctx, node); break;
    case NODE_WHILE_LOOP: gen_while_loop(ctx, node); break;
    case NODE_FOR_LOOP: gen_for_loop(ctx, node); break;
    case NODE_CONTINUE_STMT: gen_continue_stmt(ctx); break;
    case NODE_BREAK_STMT: gen_break_stmt(ctx); break;
    case NODE_CALL_STMT: gen_call_stmt(ctx, node); break;
    case NODE_FUNC_CALL: gen_func_call(ctx, node); break;
    case NODE_DATA:
    case NODE_COPERATE:
        if(node->ast.aggregate.tailing_decl) gen_var_decl(ctx, node->ast.aggregate.tailing_decl);
        break;
    case NODE_ENUM:
    case NODE_UNDEFINED: break;
    default: break;
    }
}

IrGenContext create_irgen_context(Arena *arena) {
    IrGenContext ctx = { 0 };
    memset(ctx.func_id_buckets, 0, sizeof(ctx.func_id_buckets));
    memset(ctx.label_buckets, 0, sizeof(ctx.label_buckets));
    ctx.arena = arena;
    ctx.instructions.capacity = 0;
    ctx.instructions.count = 0;
    ctx.instructions.items = NULL;
    ctx.strings.capacity = 0;
    ctx.strings.count = 0;
    ctx.strings.items = NULL;
    ctx.floats.capacity = 0;
    ctx.floats.count = 0;
    ctx.floats.items = NULL;
    ctx.next_temp = 0;
    ctx.next_label = 0;
    ctx.next_func_id = 0;
    ctx.next_slot = 0;
    ctx.in_loop = false;
    ctx.loop_end_label = -1;
    ctx.loop_start_label = -1;
    ctx.current_scope = NULL;
    return ctx;
}

void irgen_program(IrGenContext *ctx, Node *program) {
    gen_scope_push(ctx);

    NodeList* statements = program->ast.program.statements;

    for(size_t i = 0; i < statements->count; ++i) {
        gen_statement(ctx, statements->items[i]);
    }

    gen_scope_pop(ctx);
}
