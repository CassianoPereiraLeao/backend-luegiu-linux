#include "ir.h"

static IrValue gen_array_address(IrGenContext *ctx, Node *node);

static size_t primitive_size(TokenType type) {
    switch (type)
    {
    case KINT8: case KUINT8: case KCHAR: return 1;
    case KINT16: case KUINT16: case KUTFCHAR: return 2;
    case KINT32: case KUINT32: case KFLOAT: return 4;
    case KINT64: case KUINT64: case KDOUBLE: case KHEXA: case KLINK: return 8;
    default: return 8;
    }
}

static size_t sizeof_type_ir(TypecheckType type) {
    if(type.ptr_lvl > 0) return 8;
    if(type.is_vla) return 8;
    if(type.inline_def != NULL) return type.inline_def->size;

    return primitive_size(type.base);
}

static void list_push(Arena *arena, IrInstructionList *list, IrInstruction instruction) {
    if(list->count >= list->capacity) {
        size_t new_cap = list->capacity == 0 ? 16 : list->capacity * 2;
        IrInstruction* new_items = (IrInstruction*)arena_alloc(arena, sizeof(IrInstruction) * new_cap);
        if(list->count > 0) memcpy(new_items, list->items, sizeof(IrInstruction) * list->count);
        list->items = new_items;
        list->capacity = new_cap;
    }

    list->items[list->count++] = instruction;
}

static void emit(IrGenContext *ctx, IrOperators op, IrValue dest, IrValue src1, IrValue src2, int aux) {
    IrInstruction instruction = {
        op, dest,
        src1, src2, aux
    };

    list_push(ctx->arena, &ctx->instructions, instruction);
}

static IrValue none_value(void) {
    IrValue value = { 0 };
    value.kind = IR_VAL_NONE;
    return value;
}

static IrValue new_temp(IrGenContext *ctx, TypecheckType type) {
    IrValue value = { 0 };
    value.kind = IR_VAL_TEMP;
    value.as.temp_id = ctx->next_temp++;
    value.type = type;
    return value;
}

static int new_label(IrGenContext *ctx) {
    return ctx->next_label++;
}

static uint64_t hash_view(View view) {
    uint64_t hash = 14695981039346656037ULL;

    for(size_t i = 0; i < view.len; ++i) {
        hash ^= (unsigned char)view.start[i];
        hash *= 1099511628211ULL;
    }

    return hash;
}

static bool view_equals(View a, View b) {
    if(a.len != b.len) return false;
    return memcmp(a.start, b.start, b.len) == 0;
}

static IrGenScope* scope_push(IrGenContext *ctx) {
    IrGenScope* scope = (IrGenScope*)arena_alloc(ctx->arena, sizeof(IrGenScope));
    memset(scope->buckets, 0, sizeof(scope->buckets));
    scope->parent = ctx->current_scope;
    ctx->current_scope = scope;
    return scope;
}

static void scope_pop(IrGenContext *ctx) {
    ctx->current_scope = ctx->current_scope->parent;
}

static int scope_declare(IrGenContext *ctx, View name, TypecheckType type) {
    int slot = ctx->next_slot++;

    uint64_t hash = hash_view(name)& (IR_SCOPE_TABLE_SIZE - 1);

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

static int scope_declare_array(IrGenContext *ctx, View name, TypecheckType type, size_t dim_count, IrValue* strides) {
    int slot = ctx->next_slot++;
    uint64_t hash = hash_view(name) & (IR_SCOPE_TABLE_SIZE - 1);

    IrSlotEntry* entry = (IrSlotEntry*)arena_alloc(ctx->arena, sizeof(IrSlotEntry));
    entry->name = name;
    entry->slot_id = slot;
    entry->type = type;
    entry->is_array = true;
    entry->dim_count = dim_count;
    entry->dim_strides = strides;
    entry->next = ctx->current_scope->buckets[hash];
    ctx->current_scope->buckets[hash] = entry;

    return slot;
}

static IrSlotEntry* scope_lookup(IrGenContext *ctx, View name) {
    uint64_t hash = hash_view(name)& (IR_SCOPE_TABLE_SIZE - 1);

    for(IrGenScope* scope = ctx->current_scope; scope != NULL; scope = scope->parent) {
        for(IrSlotEntry* entry = scope->buckets[hash]; entry != NULL; entry = entry->next) {
            if(view_equals(entry->name, name)) return entry;
        }
    }

    return NULL;
}

static void ir_func_list_push(Arena *arena, IrFunctionRangeList *list, IrFunctionRange range) {
    if(list->count >= list->capacity) {
        size_t new_cap = list->capacity == 0 ? 8 : list->capacity * 2;
        IrFunctionRange* new_items = (IrFunctionRange*)arena_alloc(arena, sizeof(IrFunctionRange) * new_cap);
        if(list->count > 0) memcpy(new_items, list->items, sizeof(IrFunctionRange) * new_cap);
        list->items = new_items;
        list->capacity = new_cap;
    }

    list->items[list->count++] = range;
}

static void register_func_id(IrGenContext *ctx, View name, int func_id) {
    uint64_t hash = hash_view(name)& (IR_FUNC_TABLE_SIZE - 1);
    IrFuncIdEntry* entry = (IrFuncIdEntry*)arena_alloc(ctx->arena, sizeof(IrFuncIdEntry));
    entry->name = name;
    entry->func_id = func_id;
    entry->next = ctx->func_id_buckets[hash];
    ctx->func_id_buckets[hash] = entry;
}

static int lookup_func_id(IrGenContext *ctx, View name) {
    uint64_t hash = hash_view(name)& (IR_FUNC_TABLE_SIZE - 1);

    for(IrFuncIdEntry* entry = ctx->func_id_buckets[hash]; entry != NULL; entry = entry->next) {
        if(view_equals(entry->name, name)) return entry->func_id;
    }

    return -1;
}

static IrOperators binop_to_ir(TokenType op) {
    switch(op) {
        case OP_PLUS:   return IR_ADD;
        case OP_MINUS:  return IR_SUB;
        case OP_STAR:   return IR_MUL;
        case OP_SLASH:  return IR_DIV;
        case OP_MOD:    return IR_MOD;
        case OP_AND:    return IR_BAND;
        case OP_OR:     return IR_BOR;
        case OP_XOR:    return IR_BXOR;
        case OP_LSHIFT: return IR_SHL;
        case OP_RSHIFT: return IR_SHR;
        case OP_LT:     return IR_CMP_LT;
        case OP_LE:     return IR_CMP_LE;
        case OP_GT:     return IR_CMP_GT;
        case OP_GE:     return IR_CMP_GE;
        case OP_EQUALS: return IR_CMP_EQ;
        case OP_BANGEQ: return IR_CMP_NE;
        default:        return IR_ADD;
    }
}

static IrValue gen_expr(IrGenContext *ctx, Node *node);

static IrValue gen_field_access(IrGenContext *ctx, Node *node) {
    int offset = (int)node->ast.field_access.field_offset;

    if(node->ast.field_access.arrow) {
        IrValue ptr_value = gen_expr(ctx, node->ast.field_access.base);
        IrValue dest = new_temp(ctx, node->resolved_type);

        IrInstruction instruction = { 0 };
        instruction.op = IR_LOAD_INDIRECT;
        instruction.dest = dest;
        instruction.src1 = ptr_value;
        instruction.aux = offset;
        list_push(ctx->arena, &ctx->instructions, instruction);

        return dest;
    }

    IrValue value = gen_expr(ctx, node->ast.field_access.base);
    value.field_offset += offset;
    value.type = node->resolved_type;
    return value;
}

static int intern_string(IrGenContext *ctx, View text) {
    for(size_t i = 0; i < ctx->strings.count; ++i) {
        View existing = ctx->strings.items[i].text;
        if(existing.len == text.len && memcmp(existing.start, text.start, text.len) == 0) {
            return ctx->strings.items[i].id;
        }
    }

    if(ctx->strings.count == ctx->strings.capacity) {
        size_t new_cap = ctx->strings.capacity ? ctx->strings.capacity * 2 : 8;
        IrStringEntry* new_items = (IrStringEntry*)arena_alloc(ctx->arena, sizeof(IrStringEntry) * new_cap);
        if(ctx->strings.count > 0) memcpy(new_items, ctx->strings.items, sizeof(IrStringEntry) * ctx->strings.count);
        ctx->strings.items = new_items;
        ctx->strings.capacity = new_cap;
    }

    int id = (int)ctx->strings.count;
    ctx->strings.items[ctx->strings.count++] = (IrStringEntry){ text, id };
    return id;
}

static IrValue gen_literal(IrGenContext *ctx, Node *node) {
    IrValue value = { 0 };
    value.type = node->resolved_type;

    switch (node->ast.literals.type)
    {
    case INT:
    case HEXA:
        value.kind = IR_VAL_CONST_INT;
        value.as.const_i = node->ast.literals.integer64;
        break;
    case DOUBLE:
    case FLOAT:
        value.kind = IR_VAL_CONST_FLOAT;
        value.as.const_f = node->ast.literals.double64;
        break;
    case CHAR: {
        unsigned char c = (unsigned char)node->ast.literals.character.start[1];
        value.kind = IR_VAL_CONST_INT;

        if(c == '\\') {
            unsigned char next = (unsigned char)node->ast.literals.character.start[2];
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
    case STRING:
        value.kind = IR_VAL_CONST_STRING;
        value.as.string_id = intern_string(ctx, node->ast.literals.string);
        break;
    default:
        value.kind = IR_VAL_CONST_INT;
        value.as.const_i = 0;
        break;
    }

    return value;
}

static IrValue gen_var_access(IrGenContext *ctx, Node *node) {
    IrSlotEntry* entry = scope_lookup(ctx, node->ast.access_variable.name);

    IrValue value = { 0 };
    value.kind = IR_VAL_SLOT;
    value.as.slot_id = entry ? entry->slot_id : -1;

    if(entry && entry->is_array) {
        TypecheckType ptr_type = { 0 };
        ptr_type.ptr_lvl = 1;
        value.type = ptr_type;
    } else {
        value.type = node->resolved_type;
    }

    return value;
}

static IrValue gen_binary_op(IrGenContext *ctx, Node *node) {
    TokenType op = node->ast.binary_operator.op;

    if(op == OP_ASSIGN) {
        Node* left = node->ast.binary_operator.left;

        if(left->kind == NODE_FIELD_ACCESS && left->ast.field_access.arrow) {
            IrValue ptr_value = gen_expr(ctx, left->ast.field_access.base);
            IrValue src = gen_expr(ctx, node->ast.binary_operator.right);

            IrInstruction instr = { 0 };
            instr.op = IR_STORE_INDIRECT;
            instr.src1 = ptr_value;
            instr.src2 = src;
            instr.aux = (int)left->ast.field_access.field_offset;
            list_push(ctx->arena, &ctx->instructions, instr);
            return src;
        }

        if(left->kind == NODE_ARRAY) {
            IrValue address = gen_array_address(ctx, left);
            IrValue source = gen_expr(ctx, node->ast.binary_operator.right);

            IrInstruction instruction = { 0 };
            instruction.op = IR_STORE_INDIRECT;
            instruction.src1 = address;
            instruction.src2 = source;
            instruction.aux = 0;
            list_push(ctx->arena, &ctx->instructions, instruction);
            return source;
        }

        IrValue dest = gen_expr(ctx, node->ast.binary_operator.left);
        IrValue src = gen_expr(ctx, node->ast.binary_operator.right);
        emit(ctx, IR_ASSIGN, dest, src, none_value(), 0);
        return dest;
    }

    IrValue left = gen_expr(ctx, node->ast.binary_operator.left);
    IrValue right = gen_expr(ctx, node->ast.binary_operator.right);

    IrValue dest = new_temp(ctx, node->resolved_type);
    emit(ctx, binop_to_ir(op), dest, left, right, 0);
    return dest;
}

static IrValue gen_unary_op(IrGenContext *ctx, Node *node) {
    IrValue operand = gen_expr(ctx, node->ast.unary_operator.operand);

    switch (node->ast.unary_operator.op)
    {
    case OP_MINUS: {
        IrValue dest = new_temp(ctx, node->resolved_type);
        emit(ctx, IR_NEG, dest, operand, none_value(), 0);
        return dest;
    }
    case OP_BANG: {
        IrValue dest = new_temp(ctx, node->resolved_type);
        emit(ctx, IR_NOT, dest, operand, none_value(), 0);
        return dest;
    }
    case OP_PLUS_PLUS: {
        IrValue inc = { 0 };
        inc.kind = IR_VAL_CONST_INT;
        inc.as.const_i = 1;
        inc.type = operand.type;
        emit(ctx, IR_ADD, operand, operand, inc, 0);
        return operand;
    }
    case OP_MINUS_MINUS: {
        IrValue dec = { 0 };
        dec.kind = IR_VAL_CONST_INT;
        dec.as.const_i = 1;
        dec.type = operand.type;
        emit(ctx, IR_SUB, operand, operand, dec, 0);
        return operand;
    }
    default: return operand;
    }

    return operand;
}

static IrValue gen_func_call(IrGenContext *ctx, Node *node) {
    View name = node->ast.call_function.name;
    int func_id = lookup_func_id(ctx, name);

    NodeList* args = node->ast.call_function.args;
    size_t reg_count = args->count < 6 ? args->count : 6;

    for(size_t i = args->count; i > reg_count; --i) {
        IrValue arg_value = gen_expr(ctx, args->items[i - 1]);

        IrInstruction instruction = { 0 };
        instruction.op = IR_ARG_STACK;
        instruction.src1 = arg_value;
        list_push(ctx->arena, &ctx->instructions, instruction);
    }

    IrValue* reg_arg_value = (IrValue*)arena_alloc(ctx->arena, sizeof(IrValue) * (reg_count > 0 ? reg_count : 1));

    for(size_t i = 0; i < reg_count; ++i) {
        reg_arg_value[i] = gen_expr(ctx, args->items[i]);
    }

    for(size_t i = 0; i < reg_count; ++i) {
        IrInstruction instruction = { 0 };
        instruction.op = IR_ARG;
        instruction.src1 = reg_arg_value[i];
        instruction.aux = (int)i;
        list_push(ctx->arena, &ctx->instructions, instruction);
    }

    IrValue func_reference = { 0 };
    func_reference.kind = IR_VAL_FUNC;
    if(view_equals(name, (View){ "__syscall_builtin", 17 })) func_reference.kind = IR_VAL_BUILTIN;
    func_reference.as.func_id = func_id;

    IrValue dest = new_temp(ctx, node->resolved_type);

    IrInstruction call_instruction = { 0 };
    call_instruction.op = IR_CALL;
    call_instruction.dest = dest;
    call_instruction.src1 = func_reference;
    call_instruction.aux = (int)args->count;
    list_push(ctx->arena, &ctx->instructions, call_instruction);

    return dest;
}

static IrValue gen_enum_access(Node *node) {
    IrValue value;
    value.kind = IR_VAL_CONST_INT;
    value.as.const_i = node->ast.enum_access.resolved_type;
    value.type = node->resolved_type;
    return value;
}

static IrValue gen_array_address(IrGenContext *ctx, Node *node) {
    Node* indexes[MAX_ARRAY_DIMENTIONS] = { 0 };
    size_t depth = 0;

    Node* current = node;
    while(current->kind == NODE_ARRAY && depth < MAX_ARRAY_DIMENTIONS) {
        indexes[depth++] = current->ast.binary_operator.right;
        current = current->ast.binary_operator.left;
    }

    TypecheckType ptr_type = { 0 };
    ptr_type.ptr_lvl = 1;

    IrValue address = { 0 };

    if(current->kind == NODE_VAR_ACCESS) {
        IrSlotEntry* entry = scope_lookup(ctx, current->ast.access_variable.name);
        address.kind = IR_VAL_SLOT;
        address.as.slot_id = entry ? entry->slot_id : -1;
        address.type = ptr_type;
        address.field_offset = 0;

        for(size_t dimention = 0; dimention < depth; ++dimention) {
            IrValue index_value = gen_expr(ctx, indexes[depth - 1 - dimention]);
            IrValue stride = { 0 };

            if(entry && entry->is_array && dimention < entry->dim_count) {
                stride = entry->dim_strides[dimention];
            } else {
                TypecheckType pointer = entry ? entry->type : current->resolved_type;
                if(pointer.ptr_lvl > 0) pointer.ptr_lvl--;

                IrValue elem_size = { 0 };
                elem_size.kind = IR_VAL_CONST_INT;
                elem_size.as.const_i = (long long)sizeof_type_ir(pointer);
                stride = elem_size;
            }

            IrValue offset = new_temp(ctx, ptr_type);
            emit(ctx, IR_MUL, offset, index_value, stride, 0);

            IrValue new_address = new_temp(ctx, ptr_type);
            emit(ctx, IR_ADD, new_address, address, offset, 0);
            address = new_address;
        }
    } else {
        address = gen_expr(ctx, current);
        for(size_t dimention = 0; dimention < depth; ++dimention) {
            IrValue index_value = gen_expr(ctx, indexes[depth - 1 - dimention]);

            IrValue elem_size = { 0 };
            elem_size.kind = IR_VAL_CONST_INT;
            elem_size.as.const_i = (long long)sizeof_type_ir(node->resolved_type);

            IrValue offset = new_temp(ctx, ptr_type);
            emit(ctx, IR_MUL, offset, index_value, elem_size, 0);

            IrValue new_address = new_temp(ctx, ptr_type);
            emit(ctx, IR_ADD, new_address, address, offset, 0);
            address = new_address;
        }
    }

    return address;
}

static IrValue gen_array_index(IrGenContext *ctx, Node *node) {
    IrValue address = gen_array_address(ctx, node);
    IrValue dest = new_temp(ctx, node->resolved_type);
    emit(ctx, IR_LOAD_INDIRECT, dest, address, none_value(), 0);
    return dest;
}

static IrValue gen_expr(IrGenContext *ctx, Node *node) {
    switch (node->kind)
    {
    case NODE_LITERAL: return gen_literal(ctx, node);
    case NODE_VAR_ACCESS: return gen_var_access(ctx, node);
    case NODE_BINARY_OP: return gen_binary_op(ctx, node);
    case NODE_POSTFIX_OP:
    case NODE_UNARY_OP: return gen_unary_op(ctx, node);
    case NODE_FUNC_CALL: return gen_func_call(ctx, node);
    case NODE_ENUM_ACCESS: return gen_enum_access(node);
    case NODE_FIELD_ACCESS: return gen_field_access(ctx, node);
    case NODE_ARRAY: return gen_array_index(ctx, node);
    default: break;
    }

    IrValue value = { 0 };
    value.kind = IR_VAL_CONST_INT;
    return value;
}

static void gen_stmt(IrGenContext *ctx, Node *node);

static void gen_call_stmt(IrGenContext *ctx, Node *node) {
    if(node->ast.call_stmt.value) {
        IrValue value = gen_expr(ctx, node->ast.call_stmt.value);
        emit(ctx, IR_RETURN, none_value(), value, none_value(), 0);
    } else {
        emit(ctx, IR_RETURN, none_value(), none_value(), none_value(), 0);
    }
}

static void gen_array_decl(IrGenContext *ctx, Node *node) {
    TypeSpec* spec = &node->ast.decl_variable.type;
    size_t n = spec->array_dim_count;

    IrValue* dimention_values = (IrValue*)arena_alloc(ctx->arena, sizeof(IrValue) * n);
    for(size_t i = 0; i < n; ++i) {
        dimention_values[i] = gen_expr(ctx, spec->array_dims[i]);
    }

    TypecheckType elem_type = node->resolved_type;
    elem_type.is_array = false;
    elem_type.is_vla = false;

    IrValue elem_size = { 0 };
    elem_size.kind = IR_VAL_CONST_INT;
    elem_size.as.const_i = (long long)sizeof_type_ir(elem_type);

    TypecheckType size_type = { 0 };
    size_type.ptr_lvl = 1;

    IrValue* strides = (IrValue*)arena_alloc(ctx->arena, sizeof(IrValue) * n);
    strides[n - 1] = elem_size;

    for(size_t i = n - 1; i > 0; --i) {
        IrValue stride = new_temp(ctx, size_type);
        emit(ctx, IR_MUL, stride, strides[i + 1], dimention_values[i + 1], 0);
        strides[i] = stride;
    }

    IrValue total_bytes = new_temp(ctx, size_type);
    emit(ctx, IR_MUL, total_bytes, strides[0], dimention_values[0], 0);

    int slot = scope_declare_array(ctx, node->ast.decl_variable.name, node->resolved_type, n, strides);

    TypecheckType ptr_type = { 0 };
    ptr_type.ptr_lvl = 1;

    IrValue ptr_dest = { 0 };
    ptr_dest.kind = IR_VAL_SLOT;
    ptr_dest.as.slot_id = slot;
    ptr_dest.type = ptr_type;

    emit(ctx, IR_SLOT_DECL, ptr_dest, none_value(), none_value(), 0);
    emit(ctx, IR_ALLOCA, ptr_dest, total_bytes, none_value(), 0);
}

static void gen_var_decl(IrGenContext *ctx, Node *node) {
    if(node->ast.decl_variable.type.is_array) {
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
    IrValue condition = gen_expr(ctx, node->ast.if_stmt.condition);

    int label_else = new_label(ctx);
    int label_end = new_label(ctx);

    IrValue label_else_value;
    label_else_value.kind = IR_VAL_LABEL;
    label_else_value.as.label_id = label_else;
    emit(ctx, IR_JMP_IF_ZERO, none_value(), condition, label_else_value, 0);

    gen_stmt(ctx, node->ast.if_stmt.then);

    IrValue label_end_value;
    label_end_value.kind = IR_VAL_LABEL;
    label_end_value.as.label_id = label_end;
    emit(ctx, IR_JMP, none_value(), label_end_value, none_value(), 0);

    IrValue label_else_def;
    label_else_def.kind = IR_VAL_LABEL;
    label_else_def.as.label_id = label_else;
    emit(ctx, IR_LABEL, label_else_def, none_value(), none_value(), 0);

    if(node->ast.if_stmt.otherwise) gen_stmt(ctx, node->ast.if_stmt.otherwise);

    IrValue label_end_def;
    label_end_def.kind = IR_VAL_LABEL;
    label_end_def.as.label_id = label_end;
    emit(ctx, IR_LABEL, label_end_def, none_value(), none_value(), 0);
}

static bool decl_is_vla(Node *stmt) {
    if(stmt->kind != NODE_VAR_DECL) return false;
    return stmt->resolved_type.is_array && stmt->resolved_type.is_vla;
}

static bool block_has_vla(NodeList *statements) {
    for(size_t i = 0; i < statements->count; ++i) {
        if(decl_is_vla(statements->items[i])) return true;
    }

    return false;
}

static void gen_block(IrGenContext *ctx, Node *node) { 
    scope_push(ctx);

    NodeList* statements = node->ast.program.statements;
    bool has_vla = block_has_vla(statements);

    IrValue saved = { 0 };
    if(has_vla) {
        TypecheckType ptr_type = { 0 };
        ptr_type.ptr_lvl = 1;
        saved = new_temp(ctx, ptr_type);
        emit(ctx, IR_STACK_SAVE, saved, none_value(), none_value(), 0);
    }

    for(size_t i = 0; i < statements->count; ++i) {
        gen_stmt(ctx, statements->items[i]);
    }

    if(has_vla) {
        emit(ctx, IR_STACK_RESTORE, none_value(), saved, none_value(), 0);
    }

    scope_pop(ctx);
}

static void gen_func_decl(IrGenContext *ctx, Node *node) {
    node->func_id = ctx->next_func_id++;
    register_func_id(ctx, node->ast.decl_function.name, node->func_id);

    int saved_slot = ctx->next_slot;
    int saved_temp = ctx->next_temp;
    ctx->next_slot = 0;
    ctx->next_temp = 0;

    size_t start = ctx->instructions.count;

    scope_push(ctx);

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

    if(node->ast.decl_function.body && node->ast.decl_function.body->kind == NODE_BLOCK) {
        NodeList* statements = node->ast.decl_function.body->ast.program.statements;
        for(size_t i = 0; i < statements->count; ++i) {
            gen_stmt(ctx, statements->items[i]);
        }
    }

    scope_pop(ctx);

    size_t end = ctx->instructions.count;

    IrFunctionRange range;
    range.func_node = node;
    range.end = end;
    range.start = start;
    range.slot_count = ctx->next_slot;
    range.temp_count = ctx->next_temp;
    ir_func_list_push(ctx->arena, &ctx->functions, range);

    ctx->next_slot = saved_slot;
    ctx->next_temp = saved_temp;
}

static void gen_for_loop(IrGenContext *ctx, Node *node) {
    scope_push(ctx);

    if(node->ast.for_loop.init) gen_stmt(ctx, node->ast.for_loop.init);

    int label_start = new_label(ctx);
    int label_continue = new_label(ctx);
    int label_end = new_label(ctx);

    IrValue label_start_def = { 0 };
    label_start_def.kind = IR_VAL_LABEL;
    label_start_def.as.label_id = label_start;
    emit(ctx, IR_LABEL, label_start_def, none_value(), none_value(), 0);

    if(node->ast.for_loop.condition) {
        IrValue condition = gen_expr(ctx, node->ast.for_loop.condition);

        IrValue label_end_value = { 0 };
        label_end_value.kind = IR_VAL_LABEL;
        emit(ctx, IR_JMP_IF_ZERO, none_value(), condition, label_end_value, 0);
    }

    int prev_start = ctx->loop_start_label;
    int prev_end = ctx->loop_end_label;
    bool prev_in_loop = ctx->in_loop;
    ctx->loop_start_label = label_continue;
    ctx->loop_end_label = label_end;
    ctx->in_loop = true;

    gen_stmt(ctx, node->ast.for_loop.body);

    ctx->loop_start_label = prev_start;
    ctx->loop_end_label = prev_end;
    ctx->in_loop = prev_in_loop;

    IrValue label_continue_def = { 0 };
    label_continue_def.kind = IR_VAL_LABEL;
    label_continue_def.as.label_id = label_continue;
    emit(ctx, IR_LABEL, label_continue_def, none_value(), none_value(), 0);

    if(node->ast.for_loop.increment) gen_expr(ctx, node->ast.for_loop.increment);

    IrValue label_start_value = { 0 };
    label_start_value.kind = IR_VAL_LABEL;
    label_start_value.as.label_id = label_start;
    emit(ctx, IR_JMP, none_value(), label_start_value, none_value(), 0);

    IrValue label_end_def = { 0 };
    label_end_def.kind = IR_VAL_LABEL;
    label_end_def.as.label_id = label_end;
    emit(ctx, IR_LABEL, label_end_def, none_value(), none_value(), 0);

    scope_pop(ctx);
}

static void gen_while_loop(IrGenContext *ctx, Node *node) {
    int label_start = new_label(ctx);
    int label_end = new_label(ctx);

    IrValue label_start_def;
    label_start_def.kind = IR_VAL_LABEL;
    label_start_def.as.label_id = label_start;
    emit(ctx, IR_LABEL, label_start_def, none_value(), none_value(), 0);

    IrValue condition = gen_expr(ctx, node->ast.while_loop.condition);

    IrValue label_end_value;
    label_end_value.kind = IR_VAL_LABEL;
    label_end_value.as.label_id = label_end;
    emit(ctx, IR_JMP_IF_ZERO, none_value(), condition, label_end_value, 0);

    int prev_start = ctx->loop_start_label;
    int prev_end = ctx->loop_end_label;
    bool prev_in_loop = ctx->in_loop;
    ctx->loop_start_label = label_start;
    ctx->loop_end_label = label_end;
    ctx->in_loop = true;

    gen_stmt(ctx, node->ast.while_loop.body);

    ctx->loop_start_label = prev_start;
    ctx->loop_end_label = prev_end;
    ctx->in_loop = prev_in_loop;

    IrValue label_start_value;
    label_start_value.kind = IR_VAL_LABEL;
    label_start_value.as.label_id = label_start;
    emit(ctx, IR_JMP, none_value(), label_start_value, none_value(), 0);

    IrValue label_end_def;
    label_end_def.kind = IR_VAL_LABEL;
    label_end_def.as.label_id = label_end;
    emit(ctx, IR_LABEL, label_end_def, none_value(), none_value(), 0);
}

static void gen_break_stmt(IrGenContext *ctx, Node *node) {
    (void)node;
    IrValue label_value;
    label_value.kind = IR_VAL_LABEL;
    label_value.as.label_id = ctx->loop_end_label;
    emit(ctx, IR_JMP, none_value(), label_value, none_value(), 0);
}

static void gen_continue_stmt(IrGenContext *ctx, Node *node) {
    (void)node; 
    IrValue label_value;
    label_value.kind = IR_VAL_LABEL;
    label_value.as.label_id = ctx->loop_start_label;
    emit(ctx, IR_JMP, none_value(), label_value, none_value(), 0);
}

static void gen_stmt(IrGenContext *ctx, Node *node) {
    if(!node) return;

    switch (node->kind)
    {
    case NODE_VAR_DECL: gen_var_decl(ctx, node); break;
    case NODE_IF_STMT: gen_if_stmt(ctx, node); break;
    case NODE_BLOCK: gen_block(ctx, node); break;
    case NODE_BINARY_OP: gen_expr(ctx, node); break;
    case NODE_POSTFIX_OP:
    case NODE_UNARY_OP: gen_expr(ctx, node); break;
    case NODE_FUNC_DECL: gen_func_decl(ctx, node); break;
    case NODE_WHILE_LOOP: gen_while_loop(ctx, node); break;
    case NODE_CONTINUE_STMT: gen_continue_stmt(ctx, node); break;
    case NODE_BREAK_STMT: gen_break_stmt(ctx, node); break;
    case NODE_CALL_STMT: gen_call_stmt(ctx, node); break;
    case NODE_FUNC_CALL: gen_func_call(ctx, node); break;
    case NODE_ENUM: break;
    case NODE_FOR_LOOP: gen_for_loop(ctx, node); break;
    case NODE_DATA:
    case NODE_COPERATE:
        if(node->ast.aggregate.tailing_decl) gen_var_decl(ctx, node->ast.aggregate.tailing_decl);
        break;
    default: break;
    }
}

IrGenContext create_irgen_context(Arena *arena) {
    IrGenContext ctx;
    ctx.arena = arena;
    ctx.instructions.items = NULL;
    ctx.instructions.count = 0;
    ctx.instructions.capacity = 0;
    ctx.strings.items = NULL;
    ctx.strings.count = 0;
    ctx.strings.capacity = 0;
    ctx.functions.items = NULL;
    ctx.functions.count = 0;
    ctx.functions.capacity = 0;
    ctx.next_temp = 0;
    ctx.next_slot = 0;
    ctx.next_label = 0;
    ctx.next_func_id = 0;
    ctx.in_loop = false;
    ctx.loop_end_label = -1;
    ctx.loop_start_label = -1;
    ctx.current_scope = NULL;
    memset(ctx.func_id_buckets, 0, sizeof(ctx.func_id_buckets));
    return ctx;
}

void irgen_program(IrGenContext *ctx, Node *program) {
    scope_push(ctx);

    NodeList* statements = program->ast.program.statements;

    for(size_t i = 0; i < statements->count; ++i) {
        gen_stmt(ctx, statements->items[i]);
    }

    scope_pop(ctx);
}
