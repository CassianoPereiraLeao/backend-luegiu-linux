#include "typecheck.h"

static void check_var_decl(CheckContext *ctx, Node *node);
static TypecheckType resolve_type_spec(CheckContext *ctx, TypeSpec spec);
static TypecheckType check_func_call(CheckContext *ctx, Node *node);
static TypecheckType check_field_access(CheckContext *ctx, Node *node);

static TypecheckType make_type(TokenType base, size_t ptr_lvl) {
    TypecheckType type = { 0 };
    type.base = base;
    type.ptr_lvl = ptr_lvl;
    type.is_error = false;
    return type;
}

static bool view_equals(View a, View b) {
    if(a.len != b.len) return false;

    return memcmp(a.start, b.start, b.len) == 0;
}

static int alignup(int value, int alignment) {
    if(alignment <= 1) return value;
    return (value + alignment - 1)& ~(alignment - 1); 
}

static const char* base_type_name(TokenType type) {
    switch(type) {
        case KINT8:   return "int8";
        case KINT16:  return "int16";
        case KINT32:  return "int32";
        case KINT64:  return "int64";
        case KUINT8:  return "uint8";
        case KUINT16: return "uint16";
        case KUINT32: return "uint32";
        case KUINT64: return "uint64";
        case KFLOAT:  return "float";
        case KDOUBLE: return "double";
        case KCHAR:   return "char";
        case KHEXA:   return "hexa";
        case KLINK:   return "link";
        case KVOID:   return "void";
        default:      return "?";
    }
}

static char* format_type(char* buffer, size_t buffer_size, TypecheckType type) {
    size_t len;
    if(type.base == IDENTIFIER && type.custom_name.len > 0) {
        len = snprintf(buffer, buffer_size, "%.*s", (int)type.custom_name.len, type.custom_name.start);
    } else {
        len = snprintf(buffer, buffer_size, "%s", base_type_name(type.base));
    }

    for(size_t i = 0; i < type.ptr_lvl && len < buffer_size - 1; ++i) {
        buffer[len++] = '*';
    }

    buffer[len] = '\0';
    return buffer;
}

CheckContext create_check_context(Arena *arena, DiagContext *context) {
    CheckContext ctx;
    ctx.arena = arena;
    ctx.context = context;
    ctx.current_call_type = make_type(KVOID, 0);
    ctx.current_scope = NULL;
    ctx.in_function = false;
    ctx.in_loop = false;
    ctx.entry_function = NULL;
    memset(ctx.func_buckets, 0, sizeof(ctx.func_buckets));
    memset(ctx.type_buckets, 0, sizeof(ctx.type_buckets));
    return ctx;
}

static uint64_t hash_view(View view) {
    uint64_t hash = 14695981039346656037ULL;
    
    for(size_t i = 0; i < view.len; ++i) {
        hash ^= (unsigned char)view.start[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static void func_table_declare(CheckContext *ctx, View name, FuncSignature signature) {
    uint64_t hash = hash_view(name)& (FUNC_TABLE_SIZE - 1);
    FuncEntry* entry = (FuncEntry*)arena_alloc(ctx->arena, sizeof(FuncEntry));
    entry->name = name;
    entry->signature = signature;
    entry->next = ctx->func_buckets[hash];
    ctx->func_buckets[hash] = entry;
}

static FuncEntry* func_table_lookup(CheckContext *ctx, View name) {
    uint64_t hash = hash_view(name)& (FUNC_TABLE_SIZE - 1);
    
    for(FuncEntry* entry = ctx->func_buckets[hash]; entry != NULL; entry = entry->next) {
        if(view_equals(entry->name, name)) return entry;
    }

    return NULL;
}

static TypeEntry* type_table_lookup(CheckContext *ctx, View name) {
    uint64_t hash = hash_view(name)& (TYPE_TABLE_SIZE - 1);
    
    for(TypeEntry* entry = ctx->type_buckets[hash]; entry != NULL; entry = entry->next) {
        if(view_equals(entry->name, name)) return entry;
    }

    return NULL;
}

static void register_aggregate_type(CheckContext *ctx, View name, AggregateDef *def) {
    uint64_t hash = hash_view(name)& (TYPE_TABLE_SIZE - 1);

    TypeEntry* entry = (TypeEntry*)arena_alloc(ctx->arena, sizeof(TypeEntry));
    entry->name = name;
    entry->kind = TYPE_ENTRY_AGGREGATE;
    entry->aggregate = *def;
    entry->next = ctx->type_buckets[hash];
    ctx->type_buckets[hash] = entry;
}

static Scope* scope_push(CheckContext *ctx) {
    Scope* scope = (Scope*)arena_alloc(ctx->arena, sizeof(Scope));
    memset(scope->buckets, 0, sizeof(scope->buckets));
    scope->parent = ctx->current_scope;
    ctx->current_scope = scope;
    return scope;
}

static FieldInfo* find_field(AggregateDef *def, View name) {
    for(size_t i = 0; i < def->field_count; ++i) {
        if(view_equals(def->fields[i].name, name)) return &def->fields[i];
    }

    return NULL;
}

static void scope_pop(CheckContext *ctx) {
    ctx->current_scope = ctx->current_scope->parent;
}

static void scope_declare(CheckContext *ctx, View name, TypecheckType type) {
    Scope* scope = ctx->current_scope;
    uint64_t hash = hash_view(name)& (SCOPE_TABLE_SIZE - 1);

    SymbolEntry* entry = (SymbolEntry*)arena_alloc(ctx->arena, sizeof(SymbolEntry));
    entry->name = name;
    entry->type = type;
    entry->next = scope->buckets[hash];
    scope->buckets[hash] = entry;
}

static SymbolEntry* scope_find_local(Scope *scope, View name) {
    uint64_t hash = hash_view(name)& (SCOPE_TABLE_SIZE - 1);

    for(SymbolEntry* entry = scope->buckets[hash]; entry != NULL; entry = entry->next) {
        if(view_equals(entry->name, name)) return entry;
    }
    return NULL;
}

static SymbolEntry* scope_lookup(Scope *scope, View name) {
    for(Scope* s = scope; s != NULL; s = s->parent) {
        SymbolEntry* found = scope_find_local(s, name);
        if(found) return found;
    }

    return NULL;
}

static size_t primitive_size(TokenType type) {
    switch (type)
    {
    case KINT8: case KUINT8: case KCHAR: return 1;
    case KINT16: case KUINT16: case KUTFCHAR: return 2;
    case KINT32: case KUINT32: case KFLOAT: return 4;
    case KINT64: case KUINT64: case KDOUBLE: case KHEXA: case KLINK: return 8;
    default: return 0;
    }
}

static size_t type_size(CheckContext *ctx, TypecheckType type) {
    if(type.ptr_lvl > 0) return 8;
    if(type.base != IDENTIFIER) return primitive_size(type.base);
    if(type.inline_def) return type.inline_def->size;
    TypeEntry* entry = type_table_lookup(ctx, type.custom_name);
    return (entry && entry->kind == TYPE_ENTRY_AGGREGATE) ? entry->aggregate.size : 0;
}

static AggregateDef* compute_aggregate_layout(CheckContext *ctx, Node *node) {
    NodeList* members = node->ast.aggregate.members;
    AggregateDef* def = (AggregateDef*)arena_alloc(ctx->arena, sizeof(AggregateDef));
    def->kind = node->kind;
    def->field_count = members->count;
    def->fields = (FieldInfo*)arena_alloc(ctx->arena, sizeof(FieldInfo) * members->count);

    size_t running_offset = 0;
    size_t max_size = 0;

    for(size_t i = 0; i < members->count; ++i) {
        Node* field = members->items[i];
        TypecheckType field_t = resolve_type_spec(ctx, field->ast.decl_variable.type);
        size_t field_size = type_size(ctx, field_t);

        def->fields[i].name = field->ast.decl_variable.name;
        def->fields[i].type = field_t;
        def->fields[i].size = field_size;

        if(node->kind == NODE_DATA) {
            size_t field_align = field_size;
            size_t aligned_offset = alignup(running_offset, field_align);
            def->fields[i].offset = aligned_offset;
            running_offset = aligned_offset + field_size;
        } else {
            def->fields[i].offset = 0;
            if(field_size > max_size) max_size = field_size;
        }
    }

    def->size = (node->kind == NODE_DATA) ? running_offset : max_size;
    return def;
}

static TypecheckType type_error(void) {
    TypecheckType type = {0};
    type.is_error = true;
    return type;
}

static bool type_is_error(TypecheckType type) {
    return type.is_error;
}

static bool type_equals(TypecheckType a, TypecheckType b) {
    if(a.base != b.base || a.ptr_lvl != b.ptr_lvl) return false;
    if(a.base != IDENTIFIER) return true;
    if(a.custom_name.len > 0 || b.custom_name.len > 0) return view_equals(a.custom_name, b.custom_name);
    return a.inline_def == b.inline_def;
}

static bool is_ptr(TypecheckType type) {
    return type.ptr_lvl > 0 && type.base != KLINK;
}

static bool is_link(TypecheckType type) {
    return type.base == KLINK && type.ptr_lvl == 0;
}

static bool is_hexa(TypecheckType type) {
    return type.base == KHEXA && type.ptr_lvl == 0;
}

static bool is_island(TypecheckType type) {
    return is_link(type) || is_hexa(type) || is_ptr(type);
}

static bool is_integer_base(TokenType type) {
    switch (type)
    {
    case KINT8: case KINT16: case KINT32: case KINT64:
    case KUINT8: case KUINT16: case KUINT32: case KUINT64:
    case KCHAR: case KBIG: case KSMALL:
        return true;
    default: return false;
    }
}

static bool is_float_base(TokenType type) {
    return type == KFLOAT || type == KDOUBLE;
}

static bool is_unsigned_base(TokenType type) {
    switch (type)
    {
    case KUINT8: case KUINT16: case KUINT32: case KUINT64:
        return true;
    default:
        return false;
    }
}

static int integer_width(TokenType type) {
    switch (type)
    {
    case KINT8: case KUINT8: case KCHAR: return 8;
    case KINT16: case KUINT16: case KUTFCHAR: return 16;
    case KINT32: case KUINT32: return 32;
    case KINT64: case KUINT64: return 64;
    default: return 0;
    }
}

static int float_width(TokenType type) {
    switch (type)
    {
    case KFLOAT: return 32;
    case KDOUBLE: return 64;
    default: return 0;
    }
}

static bool literal_overflows(Node *node, TypecheckType type) {
    if(node->kind != NODE_LITERAL) return false;
    if(!is_integer_base(type.base)) return false;

    long long value = node->ast.literals.integer64;
    int width = integer_width(type.base);
    bool dest_unsigned = is_unsigned_base(type.base);

    if(dest_unsigned) {
        unsigned long long max = (width == 64) ? ~0ULL : ((1ULL << width) - 1);
        if(value < 0) return true;
        return (unsigned long long)value > max;
    } else {
        long long max = (width == 64) ? 0x7FFFFFFFFFFFFFFFLL : (1LL << (width - 1));
        long long min = (width == 64) ? (-max - 1) : -(1LL << (width - 1));
        return value > max || value < min;
    }
}

static CompatResult check_assignable(TypecheckType from, TypecheckType to, Node *node, bool is_cast, const char** msg) {
    if(is_cast) return COMPAT_OK;

    if(is_link(to)) {
        if(is_link(from) || is_hexa(from) || is_ptr(from)) return COMPAT_OK;
        *msg = "link so aceita 'link', 'hexa' ou ponteiros tipados sem cast explicito";
        return COMPAT_ERROR;
    }
    if(is_hexa(to)) {
        if(is_hexa(from)) return COMPAT_OK;
        *msg = "hexa so aceita outro 'hexa' sem cast explicito";
        return COMPAT_ERROR;
    }

    if(is_ptr(to)) {
        if(is_ptr(from) && type_equals(from, to)) return COMPAT_OK;
        *msg = "ponteiros de tipos diferentes exigem cast explicito";
        return COMPAT_ERROR;
    }

    if(is_link(from)) {
        *msg = "nao e possivel converter 'link' para tipo numerico sem cast";
        return COMPAT_ERROR;
    }

    if(is_hexa(from)) {
        *msg = "nao e possivel converter 'hexa' para tipo numerico sem cast";
        return COMPAT_ERROR;
    }

    if(is_ptr(from)) {
        if(!is_ptr(to)) {
            *msg = "Os dois lados precisam ser ponteiros iguais";
            return COMPAT_ERROR;
        }
        *msg = "nao e possivel converter ponteiro para tipo numerico sem cast";
        return COMPAT_ERROR;
    }

    if(type_equals(from, to)) return COMPAT_OK;

    bool from_int = is_integer_base(from.base);
    bool to_int = is_integer_base(to.base);
    bool from_float = is_float_base(from.base);
    bool to_float = is_float_base(to.base);

    if(from_int && to_int) {
        int from_width = integer_width(from.base);
        int to_width = integer_width(to.base);
        if(to_width >= from_width) return COMPAT_OK;
        
        if(node && node->kind == NODE_LITERAL) {
            if(literal_overflows(node, to)) {
                *msg = "valor do literal excede o alcance do tipo de destino";
                return COMPAT_ERROR;
            }
            return COMPAT_OK;
        }

        *msg = "possivel perca de precisao";
        return COMPAT_WARNING;
    }

    if(from_int && to_float) return COMPAT_OK;
    if(from_float && to_int) {
        *msg = "possivel perda de precisao";
        return COMPAT_WARNING;
    }

    if(from_float && to_float) {
        int from_width = float_width(from.base);
        int to_width = float_width(to.base);
        if(to_width >= from_width) return COMPAT_OK;
        *msg = "possivel perda de precisao";
        return COMPAT_WARNING;
    }

    *msg = "tipos incompativeis";
    return COMPAT_ERROR;
}

static void report_assignable(CheckContext *ctx, TypecheckType from, TypecheckType to, Node *node, Node *site) {
    const char* reason = NULL;
    CompatResult result = check_assignable(from, to, node, false, &reason);

    if(result == COMPAT_OK) return;

    char from_buffer[32], to_buffer[32];

    format_type(from_buffer, sizeof(from_buffer), from);
    format_type(to_buffer, sizeof(to_buffer), to);

    if(result == COMPAT_ERROR)
        diag_error(ctx->context, site->filename, site->line, site->col,
            "nao e possivel converter '%s' para '%s': %s", from_buffer, to_buffer, reason);
    else if(result == COMPAT_WARNING)
        diag_warning(ctx->context, site->filename, site->line, site->col, 
            "conversao de '%s' para '%s' pode causar %s", from_buffer, to_buffer, reason);
}

static TypecheckType promote_from_arith(TypecheckType a, TypecheckType b) {
    TokenType base_a = (a.base == KCHAR) ? KINT32 : a.base;
    TokenType base_b = (b.base == KCHAR) ? KINT32 : b.base;

    if(is_float_base(base_a) || is_float_base(base_b)) {
        if(base_a == KDOUBLE || base_b == KDOUBLE) return make_type(KDOUBLE, 0);
        return make_type(KFLOAT, 0);
    }

    int width_a = integer_width(base_a);
    int width_b = integer_width(base_b);
    TokenType priority = (width_a >= width_b) ? base_a : base_b;
    return make_type(priority, 0);
}

static TypecheckType check_expr(CheckContext *ctx, Node *node);

static TypecheckType check_enum_access(CheckContext *ctx, Node *node) {
    View enum_name = node->ast.enum_access.enum_name;
    View member_name = node->ast.enum_access.member_name;

    TypeEntry* entry = type_table_lookup(ctx, enum_name);
    if(!entry || entry->kind != TYPE_ENTRY_ENUM) {
        diag_error(ctx->context, node->filename, node->line, node->col,
            "'%.*s' nao e um enum declarado", (int)enum_name.len, enum_name.start);
        return type_error();
    }

    for(size_t i = 0; i < entry->enum_def.member_count; ++i) {
        if(view_equals(entry->enum_def.members[i].name, member_name)) {
            node->ast.enum_access.resolved_type = entry->enum_def.members[i].value;

            TypecheckType type = make_type(IDENTIFIER, 0);
            type.custom_name = enum_name;
            return type;
        }
    }

    diag_error(ctx->context, node->filename, node->line, node->col,
        "'%.*s' nao e um membro de '%.*s'",
        (int)member_name.len, member_name.start, (int)enum_name.len, enum_name.start);
    return type_error();
}

static void check_aggregate_decl(CheckContext *ctx, Node *node) {
    View name = node->ast.aggregate.name;

    if(node->ast.aggregate.members != NULL) {
        if(name.len > 0 && type_table_lookup(ctx, name)) {
            diag_error(ctx->context, node->filename, node->line, node->col,
                "tipo '%.*s' ja declarado", (int)name.len, name.start);
        } else {
            AggregateDef* def = compute_aggregate_layout(ctx, node);
            if(name.len > 0) register_aggregate_type(ctx, name, def);
        }
    }

    if(node->ast.aggregate.tailing_decl) check_var_decl(ctx, node->ast.aggregate.tailing_decl);
}

static void check_newtype_decl(CheckContext *ctx, Node *node) {
    View name = node->ast.newtype.name;

    if(type_table_lookup(ctx, name)) {
        diag_error(ctx->context, node->filename, node->line, node->col, 
            "tipo '%.*s' ja declarado", (int)name.len, name.start);
        return;
    }

    TypecheckType underlying = resolve_type_spec(ctx, node->ast.newtype.underlying);

    TypeEntry* entry = (TypeEntry*)arena_alloc(ctx->arena, sizeof(TypeEntry));
    entry->name = name;
    entry->kind = TYPE_ENTRY_ALIAS;
    entry->alias = underlying;

    uint64_t hash = hash_view(name)& (TYPE_TABLE_SIZE - 1);
    entry->next = ctx->type_buckets[hash];
    ctx->type_buckets[hash] = entry;
}

static TypecheckType check_literal(Node *node) {
    switch (node->ast.literals.type)
    {
    case INT: return make_type(KINT32, 0);
    case HEXA: return make_type(KHEXA, 0);
    case DOUBLE: return make_type(KDOUBLE, 0);
    case FLOAT: return make_type(KFLOAT, 0);
    case CHAR: return make_type(KCHAR, 0);
    case STRING: return make_type(KCHAR, 1);
    }
    return type_error();
}

static TypecheckType check_var_access(CheckContext *ctx, Node *node) {
    SymbolEntry* sym = scope_lookup(ctx->current_scope, node->ast.access_variable.name);
    if(!sym) {
        diag_error(ctx->context, node->filename, node->line, node->col, "variavel '%.*s' nao declarada",
            (int)node->ast.access_variable.name.len, node->ast.access_variable.name.start);
        return type_error();
    }

    return sym->type;
}

static bool is_lvalue(Node *node) {
    return node->kind == NODE_VAR_ACCESS || node->kind == NODE_ARRAY || node->kind == NODE_FIELD_ACCESS;
}

static bool is_arith_compound_op(TokenType op) {
    switch (op)
    {
    case OP_PLUSEQ: case OP_MINUSEQ: case OP_SLASHEQ: case OP_STAREQ: case OP_MODEQ:
        return true;
    default: return false;
    }
}

static bool is_bitwise_compound_op(TokenType op) {
    switch (op)
    {
    case OP_ANDEQ: case OP_OREQ: case OP_XOREQ: case OP_LSHIFTEQ: case OP_RSHIFTEQ:
        return true;
    default: return false;
    }
}

static void apply_array_info(TypecheckType *type, TypeSpec spec) {
    type->is_array = spec.is_array;
    type->is_vla = false;
    type->array_size = 0;

    if(!spec.is_array) return;

    for(size_t i = 0; i < spec.array_dim_count; ++i) {
        if(spec.array_dims[i] == NULL || spec.array_dims[i]->kind != NODE_LITERAL) {
            type->is_vla = true;
            break;
        }
    }

    if(!type->is_vla && spec.array_dim_count > 0 && spec.array_dims[0] != NULL) {
        type->array_size = (size_t)spec.array_dims[0]->ast.literals.integer64;
    }
}

static TypecheckType resolve_type_spec(CheckContext *ctx, TypeSpec spec) {
    if(spec.base != IDENTIFIER) {
        TypecheckType type = make_type(spec.base, spec.ptr_lvl);
        apply_array_info(&type, spec);
        return type;
    }

    if(spec.nested) {
        AggregateDef* def = compute_aggregate_layout(ctx, spec.nested);
        TypecheckType type = make_type(IDENTIFIER, spec.ptr_lvl);
        type.inline_def = def;

        if(spec.nested->ast.aggregate.name.len > 0) {
            register_aggregate_type(ctx, spec.nested->ast.aggregate.name, def);
            type.custom_name = spec.nested->ast.aggregate.name;
        }

        apply_array_info(&type, spec);
        return type;
    }

    TypeEntry* entry = type_table_lookup(ctx, spec.name);
    if(!entry) return type_error();

    if(entry->kind == TYPE_ENTRY_ALIAS) {
        TypecheckType type = entry->alias;
        type.ptr_lvl += spec.ptr_lvl;
        apply_array_info(&type, spec);
        return type;
    }

    TypecheckType type = make_type(IDENTIFIER, spec.ptr_lvl);
    type.custom_name = spec.name;

    if(entry->kind == TYPE_ENTRY_AGGREGATE) {
        type.inline_def = &entry->aggregate;
    }

    apply_array_info(&type, spec);
    return type;
}

static void check_stmt(CheckContext *ctx, Node *node);

static void check_enum_decl(CheckContext *ctx, Node *node) {
    View name = node->ast.enum_decl.name;

    if(type_table_lookup(ctx, name)) {
        diag_error(ctx->context, node->filename, node->line, node->col,
            "tipo '%.*s' ja declarado", (int)name.len, name.start);
        return;
    }

    NodeList* members = node->ast.enum_decl.members;
    EnumMemberInfo* infos = (EnumMemberInfo*)arena_alloc(ctx->arena, sizeof(EnumMemberInfo) * members->count);

    long long next = 0;

    for(size_t i = 0; i < members->count; ++i) {
        Node* member = members->items[i];

        if(member->ast.enum_member.value) {
            Node* value = member->ast.enum_member.value;

            if(value->kind != NODE_LITERAL || value->ast.literals.type != INT) {
                diag_error(ctx->context, value->filename, value->line, value->col,
                    "valor de membro de enum precisa ser uma constante inteira literal");
                next = 0;
            }
            else next = value->ast.literals.integer64;
        }

        infos[i].name = member->ast.enum_member.name;
        infos[i].value = next;

        next++;
    }

    TypeEntry* entry = (TypeEntry*)arena_alloc(ctx->arena, sizeof(TypeEntry));
    entry->enum_def.member_count = members->count;
    entry->enum_def.members = infos;
    entry->name = name;
    entry->kind = TYPE_ENTRY_ENUM;

    uint64_t hash = hash_view(name)& (TYPE_TABLE_SIZE - 1);
    entry->next = ctx->type_buckets[hash];
    ctx->type_buckets[hash] = entry;
}

static void check_while_loop(CheckContext *ctx, Node* node) {
    check_expr(ctx, node->ast.while_loop.condition);

    bool prev_in_loop = ctx->in_loop;
    ctx->in_loop = true;

    check_stmt(ctx, node->ast.while_loop.body);

    ctx->in_loop = prev_in_loop;
}

static void check_for_loop(CheckContext *ctx, Node *node) {
    scope_push(ctx);

    if(node->ast.for_loop.init) check_stmt(ctx, node);
    if(node->ast.for_loop.condition) check_expr(ctx, node);
    if(node->ast.for_loop.increment) check_expr(ctx, node);

    bool prev_in_loop = ctx->in_loop;
    ctx->in_loop = true;

    check_stmt(ctx, node->ast.for_loop.body);

    ctx->in_loop = prev_in_loop;

    scope_pop(ctx);
}

static void check_break_stmt(CheckContext *ctx, Node *node) {
    if(!ctx->in_loop) {
        diag_error(ctx->context, node->filename, node->line, node->col, 
            "'break' usado fora de um loop");
    }
}

static void check_continue_stmt(CheckContext *ctx, Node *node) {
    if(!ctx->in_loop) {
        diag_error(ctx->context, node->filename, node->line, node->col, 
            "'continue' usado fora de um loop");
    }
}

static TypecheckType check_binary_op(CheckContext *ctx, Node *node) {
    Node* left = node->ast.binary_operator.left;
    Node* right = node->ast.binary_operator.right;
    TypecheckType left_t = check_expr(ctx, left);
    TypecheckType right_t = check_expr(ctx, right);
    TokenType op = node->ast.binary_operator.op;

    if(type_is_error(left_t) || type_is_error(right_t)) return type_error();

    switch (op)
    {
    case OP_PLUS: case OP_MINUS: case OP_STAR: case OP_SLASH: case OP_MOD: {
        if(is_island(left_t) || is_island(right_t)) {
            diag_error(ctx->context, node->filename, node->line, node->col,
                "operador aritmetico exige tipos numericos");
            return type_error();
        }

        if(op == OP_MOD && (is_float_base(left_t.base) || is_float_base(right_t.base))) {
            diag_error(ctx->context, node->filename, node->line, node->col,
                "'%%' exige operandos inteiros");
            return type_error();
        }

        return promote_from_arith(left_t, right_t);
    }

    case OP_LT: case OP_GT: case OP_LE: case OP_GE:
    case OP_EQUALS: case OP_BANGEQ: {
        if(is_island(left_t) != is_island(right_t)) {
            diag_error(ctx->context, node->filename, node->line, node->col,
                "operandos incompativeis na comparacao");
            return type_error();
        }

        if(is_island(left_t) && !type_equals(left_t, right_t) &&
            !(is_link(left_t) && is_ptr(right_t)) && !(is_ptr(left_t) && is_link(right_t))) {
            diag_error(ctx->context, node->filename, node->line, node->col, 
                "comparacao entre tipos de endereco incompativeis");
            return type_error();
        }

        return make_type(KINT8, 0);
    }

    case OP_LOGAND: case OP_LOGOR: {
        
        return make_type(KINT8, 0);
    }

    case OP_AND: case OP_OR: case OP_XOR: case OP_LSHIFT: case OP_RSHIFT: {
        if(!is_integer_base(left_t.base) || !is_integer_base(right_t.base)) {
            diag_error(ctx->context, node->filename, node->line, node->col, 
                "operador bit a bit exige tipos inteiros");
            return type_error();
        }

        return promote_from_arith(left_t, right_t);
    }

    case OP_ASSIGN: {
        if(!is_lvalue(left)) {
            diag_error(ctx->context, node->filename, node->line, node->col, 
                "lado esquerdo da atribuicao precisa ser uma variavel");
            return type_error();
        }

        report_assignable(ctx, right_t, left_t, right, node);
        return left_t;
    }

    default:
        if(is_arith_compound_op(op)) {
            if(!is_lvalue(left)) {
                diag_error(ctx->context, node->filename, node->line, node->col,
                    "lado esquerdo de atribuicao composta precisa ser uma variavel");
                return type_error();
            }

            if(is_island(left_t) || is_island(right_t)) {
                diag_error(ctx->context, node->filename, node->line, node->col, 
                    "operador aritmetico composto exige tipos numericos");
                return type_error();
            }

            if(op == OP_MODEQ && (is_float_base(left_t.base) || is_float_base(right_t.base))) {
                diag_error(ctx->context, node->filename, node->line, node->col,
                    "'%%=' exige operandos inteiros");
                return type_error();
            }

            TypecheckType result = promote_from_arith(left_t, right_t);
            report_assignable(ctx, result, left_t, NULL, node);
            return left_t;
        }

        if(is_bitwise_compound_op(op)) {
            if(!is_lvalue(left)) {
                diag_error(ctx->context, node->filename, node->line, node->col,
                    "lado esquerdo de atribuicao composta precisa ser uma variavel");
                return type_error();
            }

            if(!is_integer_base(left_t.base) || !is_integer_base(right_t.base)) {
                diag_error(ctx->context, node->filename, node->line, node->col,
                    "operador bit a bit composto exige tipos inteiros");
                return type_error();
            }

            TypecheckType result = promote_from_arith(left_t, right_t);
            report_assignable(ctx, result, left_t, NULL, node);
            return left_t;
        }
        
        diag_error(ctx->context, node->filename, node->line, node->col, 
            "operador desconhecido");
        return type_error();
    }
}

static TypecheckType check_array(CheckContext *ctx, Node *node) {
    Node* left = node->ast.binary_operator.left;
    Node* right = node->ast.binary_operator.right;

    TypecheckType base = check_expr(ctx, left);
    TypecheckType index = check_expr(ctx, right);

    if(type_is_error(base) || type_is_error(index)) return type_error();

    if(base.ptr_lvl == 0) {
        diag_error(ctx->context, node->filename, node->line, node->col,
            "indexacao '[]' so pode ser aplicada a um ponteiro ou array");
        return type_error();
    }

    if(index.ptr_lvl > 0 || !is_integer_base(index.base)) {
        diag_error(ctx->context, node->filename, node->line, node->col,
            "indice de array precisa ser um valor inteiro");
        return type_error();
    }

    TypecheckType result = base;
    result.ptr_lvl--;
    return result;
}

static TypecheckType check_unary_op(CheckContext *ctx, Node *node) {
    TypecheckType operand = check_expr(ctx, node->ast.unary_operator.operand);
    if(type_is_error(operand)) return type_error();

    switch (node->ast.unary_operator.op)
    {
    case OP_MINUS:
        if(is_island(operand)) {
            diag_error(ctx->context, node->filename, node->line, node->col, 
                "operador unario '-' exige tipo numerico");
            return type_error();
        }
        return operand;

    case OP_BANG:
        return make_type(KINT8, 0);

    case OP_AND:
        return make_type(operand.base, operand.ptr_lvl + 1);
    
    case OP_STAR:
        if(operand.ptr_lvl == 0) {
            diag_error(ctx->context, node->filename, node->line, node->col,
                "nao e possivel dereferenciar um valor que nao e ponteiro");
            return type_error();
        }
        return make_type(operand.base, operand.ptr_lvl - 1);
    
    case OP_PLUS_PLUS: case OP_MINUS_MINUS:
        if(is_island(operand)) {
            diag_error(ctx->context, node->filename, node->line, node->col, 
                "'++'/'--' exige tipo numerico");
            return type_error();
        }

        return operand;

    default:
        return operand;
    }
}

static TypecheckType check_expr(CheckContext *ctx, Node *node) {
    if(!node) return type_error();

    TypecheckType result = { 0 };
    switch (node->kind)
    {
    case NODE_LITERAL: result = check_literal(node); break;
    case NODE_VAR_ACCESS: result = check_var_access(ctx, node); break;
    case NODE_BINARY_OP: result = check_binary_op(ctx, node); break;
    case NODE_UNARY_OP: result = check_unary_op(ctx, node); break;
    case NODE_POSTFIX_OP: result = check_unary_op(ctx, node); break;
    case NODE_FUNC_CALL: result = check_func_call(ctx, node); break;
    case NODE_ENUM_ACCESS: result = check_enum_access(ctx, node); break;
    case NODE_FIELD_ACCESS: result = check_field_access(ctx, node); break;
    case NODE_ARRAY: result = check_array(ctx, node); break;
    default:
        diag_error(ctx->context, node->filename, node->line, node->col, 
            "expressao nao suportada ainda pelo typechecker");
        return type_error();
    }

    node->resolved_type = result;
    return result;
}

static bool typespec_has_vla(TypeSpec *spec) {
    for(size_t i = 0; i < spec->array_dim_count; ++i) {
        if(spec->array_dims[i] && spec->array_dims[i]->kind != NODE_LITERAL) return true;
    }
    return false;
}

static void check_var_decl(CheckContext *ctx, Node *node) {
    View name = node->ast.decl_variable.name;

    if(scope_find_local(ctx->current_scope, name)) {
        diag_error(ctx->context, node->filename, node->line, node->col,
            "variavel '%.*s' ja declarada neste escopo", (int)name.len, name.start);
    }

    TypeSpec* spec = &node->ast.decl_variable.type;
    if(spec->is_array) {
        for(size_t i = 0; i < spec->array_dim_count; ++i) {
            Node* dimention = spec->array_dims[i];

            if(dimention == NULL) {
                if(i != 0) {
                    diag_error(ctx->context, node->filename, node->line, node->col,
                        "somente a primeira dimensao do array pode ser omitida");
                }
                continue;
            }

            TypecheckType dimention_type = check_expr(ctx, dimention);
            if(!is_integer_base(dimention_type.base)) {
                diag_error(ctx->context, node->filename, node->line, node->col,
                    "tamanho de array deve ser um inteiro");
            }
        }

        if(typespec_has_vla(spec)) {
            if(node->ast.decl_variable.stattic) {
                diag_error(ctx->context, node->filename, node->line, node->col,
                    "array de tamanho variavel nao pode ser 'static'");
            }

            if(!ctx->in_function) {
                diag_error(ctx->context, node->filename, node->line, node->col,
                    "array de tamanho variavel so e permitido dentro de funcoes");
            }
        }
    }

    TypecheckType declared = resolve_type_spec(ctx, node->ast.decl_variable.type);
    if(type_is_error(declared)) {
        diag_error(ctx->context, node->filename, node->line, node->col,
            "tipo nao declarado para '%.*s'", (int)name.len, name.start);
    }

    if(node->ast.decl_variable.init) {
        TypecheckType current = check_expr(ctx, node->ast.decl_variable.init);
        report_assignable(ctx, current, declared, node->ast.decl_variable.init, node);
    }

    node->resolved_type = declared;
    scope_declare(ctx, name, declared);
}

static void check_if_stmt(CheckContext *ctx, Node *node) {
    check_expr(ctx, node->ast.if_stmt.condition);
    check_stmt(ctx, node->ast.if_stmt.then);
    if(node->ast.if_stmt.otherwise) check_stmt(ctx, node->ast.if_stmt.otherwise);
}

static void check_block(CheckContext *ctx, Node *node) {
    scope_push(ctx);
    NodeList* statements = node->ast.program.statements;

    for(size_t i = 0; i < statements->count; ++i) {
        check_stmt(ctx, statements->items[i]);
    }

    scope_pop(ctx);
}

static void check_func_decl(CheckContext *ctx, Node *node) {
    if(ctx->in_function) {
        diag_error(ctx->context, node->filename, node->line, node->col,
            "funcoes aninhadas nao sao permitidas");
        return;
    }

    View name = node->ast.decl_function.name;

    if(func_table_lookup(ctx, name)) {
        diag_error(ctx->context, node->filename, node->line, node->col,
            "funcao '%.*s' ja declarada", (int)name.len, name.start);
        return;
    }

    TypecheckType call_type = resolve_type_spec(ctx, node->ast.decl_function.call_type);

    if(type_is_error(call_type)) {
        diag_error(ctx->context, node->filename, node->line, node->col, 
            "tipo de retorno nao declarado para '%.*s'", (int)name.len, name.start); 
    }

    NodeList* params = node->ast.decl_function.params;
    TypecheckType* param_t = (TypecheckType*)arena_alloc(ctx->arena, sizeof(TypecheckType) * params->count);

    for(size_t i = 0; i < params->count; ++i) {
        Node* param = params->items[i];
        param_t[i] = resolve_type_spec(ctx, param->ast.decl_variable.type);
        param->resolved_type = param_t[i];
    }

    FuncSignature signature;
    signature.call_type = call_type;
    signature.param_types = param_t;
    signature.param_count = params->count;
    signature.variadic = node->ast.decl_function.variadic;

    func_table_declare(ctx, name, signature);

    scope_push(ctx);

    for(size_t i = 0; i < params->count; ++i) {
        Node* param = params->items[i];
        scope_declare(ctx, param->ast.decl_variable.name, param_t[i]);
    }

    ctx->current_call_type = call_type;
    ctx->in_function = true;

    if(node->ast.decl_function.body && node->ast.decl_function.body->kind == NODE_BLOCK) {
        NodeList* stmts = node->ast.decl_function.body->ast.program.statements;
        for(size_t i = 0; i < stmts->count; ++i) {
            check_stmt(ctx, stmts->items[i]);
        }
    }

    ctx->in_function = false;

    if(view_equals(name, (View){ "start", 5 })) {
        if(ctx->entry_function != NULL) {
            diag_error(ctx->context, node->filename, node->line, node->col,
                "multiplas definicoes de entry point ('start')");
        }
        else ctx->entry_function = node;
    }

    scope_pop(ctx);
}

static TypecheckType check_func_call(CheckContext *ctx, Node *node) {
    View name = node->ast.call_function.name;
    FuncEntry* entry = func_table_lookup(ctx, name);

    if(!entry) {
        diag_error(ctx->context, node->filename, node->line, node->col,
            "funcao '%.*s' nao declarada", (int)name.len, name.start);
        return type_error();
    }

    FuncSignature* signature = &entry->signature;
    NodeList* args = node->ast.call_function.args;

    if(args->count < signature->param_count || (!signature->variadic && args->count > signature->param_count)) {
        diag_error(ctx->context, node->filename, node->line, node->col,
            "numero incorreto de argumentos para '%.*s': esperado %zu, recebido %zu",
            (int)name.len, name.start, signature->param_count, args->count);
        return signature->call_type;
    }

    for(size_t i = 0; i < signature->param_count; ++i) {
        if(view_equals(name, (View){ "__syscall_builtin", 17 })) break;
        Node* arg_node = args->items[i];
        TypecheckType arg_type = check_expr(ctx, arg_node);
        report_assignable(ctx, arg_type, signature->param_types[i], arg_node, arg_node);
    }

    for(size_t i = signature->param_count; i < args->count; ++i) {
        check_expr(ctx, args->items[i]);
    }

    return signature->call_type;
}

static void check_call_stmt(CheckContext *ctx, Node *node) {
    if(!ctx->in_function) {
        diag_error(ctx->context, node->filename, node->line, node->col, 
            "'call' fora de uma funcao");
        return;
    }

    bool call_void = (ctx->current_call_type.base == KVOID && ctx->current_call_type.ptr_lvl == 0);

    if(node->ast.call_stmt.value) {
        if(call_void) {
            diag_error(ctx->context, node->filename, node->line, node->col, 
                "funcao 'void' nao pode retornar um valor");
            return;
        }
        TypecheckType current = check_expr(ctx, node->ast.call_stmt.value);
        report_assignable(ctx, current, ctx->current_call_type, node->ast.call_stmt.value, node);
    } else {
        diag_error(ctx->context, node->filename, node->line, node->col, 
            "funcao espera um valor de retorno");
    }
}

static TypecheckType check_field_access(CheckContext *ctx, Node *node) {
    check_expr(ctx, node->ast.field_access.base);

    Node* base = node->ast.field_access.base;
    TypecheckType type = base->resolved_type;
    bool arrow = node->ast.field_access.arrow;

    if(arrow && type.ptr_lvl == 0) {
        diag_error(ctx->context, node->filename, node->line, node->col,
            "'->' exige um ponteiro de 1 nivel");
        return type_error();
    }

    if(!arrow && type.ptr_lvl != 0) {
        diag_error(ctx->context, node->filename, node->line, node->col,
            "use '->' para acessar campo atraves de ponteiro, nao '.'");
        return type_error();
    }

    AggregateDef* def = type.inline_def;
    if(!def) {
        diag_error(ctx->context, node->filename, node->line, node->col,
            "tipo base nao e data/coperate");
        return type_error();
    }

    FieldInfo* field = find_field(def, node->ast.field_access.field_name);
    if(!field) {
        diag_error(ctx->context, node->filename, node->line, node->col,
            "campo '%.*s' nao existe no tipo",
            (int)node->ast.field_access.field_name.len,
            node->ast.field_access.field_name.start);
        return type_error();
    }

    node->ast.field_access.field_offset = field->offset;
    node->ast.field_access.base_type = type;
    return field->type;
}

static void check_stmt(CheckContext *ctx, Node *node) {
    if(!node) return;
    switch (node->kind)
    {
    case NODE_VAR_DECL: check_var_decl(ctx, node); break;
    case NODE_IF_STMT: check_if_stmt(ctx, node); break;
    case NODE_BLOCK: check_block(ctx, node); break;
    case NODE_FUNC_DECL: check_func_decl(ctx, node); break;
    case NODE_CALL_STMT: check_call_stmt(ctx, node); break;
    case NODE_BREAK_STMT: check_break_stmt(ctx, node); break;
    case NODE_FOR_LOOP: check_for_loop(ctx, node); break;
    case NODE_CONTINUE_STMT: check_continue_stmt(ctx, node); break;
    case NODE_WHILE_LOOP: check_while_loop(ctx, node); break;
    case NODE_FUNC_CALL: check_func_call(ctx, node); break;
    case NODE_DATA:
    case NODE_COPERATE: check_aggregate_decl(ctx, node); break;
    case NODE_BINARY_OP: check_expr(ctx, node); break;
    case NODE_NEWTYPE: check_newtype_decl(ctx, node); break;
    case NODE_ENUM: check_enum_decl(ctx, node); break;
    case NODE_FIELD_ACCESS: check_field_access(ctx, node); break;
    default:
        break;
    }
}

static void register_builtin(CheckContext *ctx) {
    View name = { "__syscall_builtin", 17 };

    TypecheckType* params = (TypecheckType*)arena_alloc(ctx->arena, sizeof(TypecheckType) * 7);

    params[0].base = KINT64;
    params[0].ptr_lvl = 0;
    params[1].base = KINT64;
    params[1].ptr_lvl = 0;
    params[2].base = KINT64;
    params[2].ptr_lvl = 0;
    params[3].base = KINT64;
    params[3].ptr_lvl = 0;
    params[4].base = KINT64;
    params[4].ptr_lvl = 0;
    params[5].base = KINT64;
    params[5].ptr_lvl = 0;
    params[6].base = KINT64;
    params[6].ptr_lvl = 0;

    TypecheckType type = { 0 };
    type.base = KINT64;
    type.ptr_lvl = 0;

    FuncSignature signature = { 0 };
    signature.call_type = type;
    signature.param_count = 7;
    signature.param_types = params;
    signature.variadic = false;

    func_table_declare(ctx, name, signature);
}

void check_program(CheckContext *ctx, Node *program) {
    register_builtin(ctx);

    scope_push(ctx);
    NodeList* stmts = program->ast.program.statements;

    for(size_t i = 0; i < stmts->count; ++i) {
        check_stmt(ctx, stmts->items[i]);
    }

    scope_pop(ctx);

    if(ctx->entry_function == NULL) {
        diag_error(ctx->context, program->filename, program->line, program->col,
            "programa sem entry point 'start'");
    }
}
