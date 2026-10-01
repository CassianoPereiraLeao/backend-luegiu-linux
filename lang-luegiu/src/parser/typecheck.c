#include "../../include/typecheck.h"

static void check_var_decl(CheckContext *ctx, Node *node);
static Typecheck resolve_type_spec(CheckContext *ctx, TypeSpec spec, Node *node);
static void check_enum_decl(CheckContext *ctx, Node *node);

static Typecheck make_type(TokenType base, size_t ptr_lvl) {
    Typecheck type = { 0 };
    type.ptr_lvl = ptr_lvl;
    type.base = base;
    type.error = false;
    return type;
}

static const char* base_type_name(TokenType type) {
    switch (type)
    {
    case KINT8: return "int8";
    case KINT16: return "int16";
    case KINT32: return "int32";
    case KINT64: return "int64";
    case KUINT8: return "uint8";
    case KUINT16: return "uint16";
    case KUINT32: return "uint32";
    case KUINT64: return "uint64";
    case KHEXA: return "hexa";
    case KFLOAT: return "float";
    case KDOUBLE: return "double";
    case KLINK: return "link";
    case KVOID: return "void";
    case KCHAR: return "char";
    case KUTFCHAR: return "utfchar";
    default: return "?";
    }
}

static char* format_type(char* buffer, size_t buffer_size, Typecheck type) {
    size_t len = 0;
    if(type.base == IDENTIFIER && type.alias.len > 0)
        len = snprintf(buffer, buffer_size, "%.*s", (int)type.alias.len, type.alias.start);
    else len = snprintf(buffer, buffer_size, "%s", base_type_name(type.base));

    for(size_t i = 0; i < type.ptr_lvl && len < buffer_size - 1; ++i)
        buffer[len++] = '*';

    if(len >= buffer_size) len = buffer_size - 1;
    buffer[len] = '\0';
    return buffer;
}

static void ctx_scope_push(CheckContext *ctx) {
    ctx->current_scope = scope_push(ctx->arena, ctx->current_scope);
}

static void ctx_scope_pop(CheckContext *ctx) {
    ctx->current_scope = ctx->current_scope->next;
}

static SymbolEntry* ctx_scope_declare(CheckContext *ctx, View name, Typecheck type) {
    return scope_insert(ctx->arena, ctx->current_scope, name, type);
}

static FieldInfo* find_field(AggregateDef *def, View name) {
    for(size_t i = 0; i < def->count; ++i) {
        if(view_equals_view(def->fields[i].name, name)) return &def->fields[i];
    }

    return NULL;
}

static size_t primitive_size(TokenType type) {
    switch (type)
    {
    case KINT8: case KUINT8: case KCHAR: return 1;
    case KINT16: case KUINT16: case KUTFCHAR: return 2;
    case KINT32: case KUINT32: case KFLOAT: return 4;
    case KINT64: case KUINT64: case KDOUBLE: case KLINK: case KHEXA: return 8;
    default: return 0;
    }
}

static size_t type_size(Typecheck type) {
    size_t size;

    if(type.ptr_lvl > 0) {
        size = 8;
    }
    else if(type.base != IDENTIFIER) {
        size = primitive_size(type.base);
    }
    else if(type.inline_def) {
        size = type.inline_def->size;
    }
    else {
        TypeEntry* entry = type_table_lookup(type.alias);

        if(!entry || entry->kind != TYPE_ENTRY_AGGREGATE)
            return 0;

        size = entry->aggregate.size;
    }

    if(type._array) {
        if(type._vla)
            return 0;

        size *= type.array_size;
    }

    return size;
}

static AggregateDef* compute_aggregate_layout(CheckContext *ctx, Node *node) {
    NodeList* members = node->ast.aggregate.members;
    AggregateDef* def = (AggregateDef*)arena_alloc(ctx->arena, sizeof(AggregateDef));
    def->kind = node->kind;
    def->count = members->count;
    def->fields = (FieldInfo*)arena_alloc(ctx->arena, sizeof(FieldInfo) * members->count);

    size_t running_offset = 0;
    size_t max_size = 0;

    for(size_t i = 0; i < members->count; ++i) {
        Node* field = members->items[i];
        Typecheck field_type = resolve_type_spec(ctx, field->ast.decl_variable.call_type, field);
        size_t field_size = type_size(field_type);
        
        def->fields[i].name = field->ast.decl_variable.name;
        def->fields[i].type = field_type;
        def->fields[i].size = field_size;

        if(node->kind == NODE_DATA) {
            size_t field_align = field_size;
            size_t aligned = align_size(running_offset, field_align);
            def->fields[i].offset = aligned;
            running_offset = aligned + field_size;
        } else {
            def->fields[i].offset = 0;
            if(field_size > max_size) max_size = field_size;
        }
    }

    def->size = (node->kind == NODE_DATA) ? running_offset : max_size;
    return def;
}

static Typecheck type_error(void) {
    Typecheck type = { 0 };
    type.error = true;
    return type;
}

static bool type_is_error(Typecheck type) {
    return type.error;
}

static bool type_equals(Typecheck first, Typecheck second) {
    if(first.base != second.base || first.ptr_lvl != second.ptr_lvl) return false;
    if(first.base != IDENTIFIER) return true;
    if(first.alias.len > 0 || second.alias.len > 0) return view_equals_view(first.alias, second.alias);
    return first.inline_def == second.inline_def;
}

static bool isptr(Typecheck type) {
    return type.ptr_lvl > 0 && type.base != KLINK;
}

static bool islink(Typecheck type) {
    return type.base == KLINK && type.ptr_lvl == 0;
}

static bool ishexatype(Typecheck type) {
    return type.base == KHEXA && type.ptr_lvl == 0;
}

static bool isisland(Typecheck type) {
    return ishexatype(type) || islink(type) || isptr(type);
}

static bool isintegerbase(TokenType type) {
    switch (type)
    {
    case KINT8: case KINT16: case KINT32: case KINT64:
    case KUINT8: case KUINT16: case KUINT32: case KUINT64:
    case KCHAR: case KBIG: case KSMALL:
        return true;
    default: return false;
    }
}

static bool isfloatbase(TokenType type) {
    return type == KFLOAT || type == KDOUBLE;
}

static bool isunsignedbase(TokenType type) {
    switch (type)
    {
    case KUINT8: case KUINT16: case KUINT32: case KUINT64:
        return true;
    default: return false;
    }
}

static int integerwidth(TokenType type) {
    switch (type)
    {
    case KINT8: case KUINT8: case KCHAR: case KSMALL: return 8;
    case KINT16: case KUINT16: return 16;
    case KINT32: case KUINT32: return 32;
    case KINT64: case KUINT64: case KBIG: return 64;
    default: return 0;
    }
}

static int floatwidth(TokenType type) {
    switch (type)
    {
    case KFLOAT: return 32;
    case KDOUBLE: return 64;
    default: return 0;
    }
}

static bool literal_overflow(Node *node, Typecheck type) {
    if(node->kind != NODE_LITERAL) return false;
    if(!isintegerbase(type.base)) return false;

    LiteralKind kind = node->ast.literal.kind;
    if(kind != INT && kind != HEXA && kind != BIG && kind != UNSIGNED) return false;

    long long value;
    if(kind == HEXA || kind == UNSIGNED) {
        size_t raw = node->ast.literal.unsigned64;
        if(raw > (size_t)0x7FFFFFFFFFFFFFFFLL) return true;
        value = (long long) raw;
    } else {
        value = node->ast.literal.integer64;
    }

    int width = integerwidth(type.base);
    bool dest_unsigned = isunsignedbase(type.base);

    if(dest_unsigned) {
        // Informação importante: usando size_t pois por baixo dos panos é um unsigned long long, evita escrever longo e repetido
        size_t max = (width == 64) ? ~0ULL : ((1ULL << width) - 1);
        if(value < 0) return true;
        return (size_t)value > max;
    }

    long long max = (width == 64) ? 0x7FFFFFFFFFFFFFFFLL : (1LL << (width - 1)) - 1;
    long long min = (width == 64) ? (-max - 1) : -(1LL << (width - 1));
    return value > max || value < min;
}

static bool signature_equal(FunctionSignature *first, FunctionSignature* second) {
    if(!type_equals(first->call_type, second->call_type)) return false;
    if(first->param_count != second->param_count) return false;
    if(first->variadic != second->variadic) return false;

    for(size_t i = 0; i < first->param_count; ++i) {
        if(!type_equals(first->param_types[i], second->param_types[i])) return false;
    }

    return true;
}

static CompatResult check_assignable(Typecheck from, Typecheck to, Node *node, bool _cast, const char** msg) {
    if(_cast) return COMPAT_OK;

    if(islink(to)) {
        if(islink(from) || ishexatype(from) || isptr(from)) return COMPAT_OK;
        *msg = "link so aceita 'link', 'hexa' ou ponteiros tipados sem cast explicito";
        return COMPAT_ERROR;
    }

    if(ishexatype(to)) {
        if(ishexatype(from)) return COMPAT_OK;
        *msg = "hexa so aceita outro 'hexa' sem cast explicito";
        return COMPAT_ERROR;
    }

    if(isptr(to)) {
        if(islink(from)) return COMPAT_OK;
        if((isptr(from)) && type_equals(from, to)) return COMPAT_OK;
        *msg = "ponteiros de tipos diferentes exigem cast explicito";
        return COMPAT_ERROR;
    }

    if(islink(from)) {
        *msg = "nao e possivel converter 'link' para tipo numerico sem cast";
        return COMPAT_ERROR;
    }

    if(ishexatype(from)) {
        *msg = "nao e possivel converter 'hexa' para tipo numerico sem cast";
        return COMPAT_ERROR;
    }

    if(isptr(from)) {
        if(!isptr(to)) {
            *msg = "Os dois lados precisam ser ponteiros iguais";
            return COMPAT_ERROR;
        }
        *msg = "nao e possivel converter ponteiro para tipo numerico sem cast";
        return COMPAT_ERROR;
    }

    if(type_equals(from, to)) return COMPAT_OK;

    bool from_int = isintegerbase(from.base);
    bool to_int = isintegerbase(to.base);
    bool from_float = isfloatbase(from.base);
    bool to_float = isfloatbase(to.base);

    if(from_int && to_int) {
        int from_width = integerwidth(from.base);
        int to_width = integerwidth(to.base);
        if(to_width >= from_width) return COMPAT_OK;

        if(node != NODE_UNDEFINED && node->kind == NODE_LITERAL) {
            if(literal_overflow(node, to)) {
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
        *msg = "possivel perca de precisao";
        return COMPAT_WARNING;
    }

    if(from_float && to_float) {
        int from_width = floatwidth(from.base);
        int to_width = floatwidth(to.base);
        if(to_width && from_width) return COMPAT_OK;
        *msg = "possivel perca de precisao";
        return COMPAT_WARNING;
    }

    *msg = "tipos incompativeis";
    return COMPAT_ERROR;
}

static void report_assignable(CheckContext *ctx, Typecheck from, Typecheck to, Node *node, Node *site) {
    const char* reason = NULL;
    CompatResult result = check_assignable(from, to, node, false, &reason);

    if(result == COMPAT_OK) return;

    char from_buffer[512], to_buffer[512];

    format_type(from_buffer, sizeof(from_buffer), from);
    format_type(to_buffer, sizeof(to_buffer), to);

    if(result == COMPAT_ERROR)
        diag_error(ctx->ctx, site->filename, site->line, site->col, 
            "Nao e possivel converter de '%s' para '%s': %s", from_buffer, to_buffer, reason);
    if(result == COMPAT_WARNING)
        diag_warning(ctx->ctx, site->filename, site->line, site->col, 
            "Conversao de '%s' para '%s' pode causar '%s'", from_buffer, to_buffer, reason);
}

static Typecheck promote_from_arith(Typecheck first, Typecheck second) {
    TokenType first_base = (first.base == KCHAR) ? KUINT8 : first.base;
    TokenType second_base = (second.base == KCHAR) ? KUINT8 : first.base;

    if(isfloatbase(first_base) || isfloatbase(second_base)) {
        if(first_base == KDOUBLE || second_base == KDOUBLE) return make_type(KDOUBLE, 0);
        return make_type(KFLOAT, 0);
    }

    int first_width = integerwidth(first.base);
    int second_width = integerwidth(second.base);
    TokenType priority = (first_width > second_width) ? first_base : second_base;
    return make_type(priority, 0);
}

static bool islvalue(Node *node) {
    return node->kind == NODE_VAR_CALL || node->kind == NODE_ARRAY || node->kind == NODE_FIELD_CALL;
}

bool isarith_compoundop(TokenType op) {
    switch (op)
    {
    case OP_PLUSEQ: case OP_MINUSEQ: case OP_SLASHEQ:
    case OP_STAREQ: case OP_MODEQ:
        return true;
    default: return false;
    }
}

bool isbitwise_compoundop(TokenType op) {
    switch (op)
    {
    case OP_ANDEQ: case OP_XOREQ: case OP_OREQ:
    case OP_RSHIFTEQ: case OP_LSHIFTEQ:
        return true;
    default: return false;
    }
}

static bool dimention_isommited(Node *dimentions) {
    return dimentions == NULL || dimentions->kind == NODE_UNDEFINED;
}

static void apply_array_info(Typecheck *type, TypeSpec spec) {
    type->_array = spec.is_array;
    type->_vla = false;
    type->array_size = 0;

    if(!spec.is_array || !spec.array_dimentions) return;

    NodeList* dimentions = spec.array_dimentions;

    for(size_t i = 0; i < dimentions->count; ++i) {
        Node* dimention = dimentions->items[i];
        if(dimention_isommited(dimention) || dimention->kind != NODE_LITERAL) {
            type->_vla = true;
            break;
        }
    }

    if(!type->_vla && dimentions->count > 0 && !dimention_isommited(dimentions->items[0])) {
        type->array_size = (size_t)dimentions->items[0]->ast.literal.integer64;
    }
}

static bool typespec_has_vla(TypeSpec *spec) {
    if(!spec->array_dimentions) return false;

    for(size_t i = 0; i < spec->array_dimentions->count; ++i) {
        Node* dimention = spec->array_dimentions->items[i];
        if(dimention_isommited(dimention) || dimention->kind != NODE_LITERAL) return true;
    }

    return false;
}

static Typecheck check_expr(CheckContext *ctx, Node *node);

static Typecheck check_bytes(CheckContext *ctx, Node *node) {
    Typecheck type = resolve_type_spec(ctx, node->ast.bytes.type, node);

    if(type_is_error(type)) {
        diag_error( ctx->ctx, node->filename, node->line, node->col,
            "Tipo invalido para 'bytes'");
        return type_error();
    }

    size_t size = type_size(type);

    if(size == 0) {
        diag_error( ctx->ctx, node->filename, node->line, node->col,
            "Nao foi possivel determinar o tamanho do tipo");
        return type_error();
    }

    node->ast.bytes.size = size;
    return make_type(KUINT64, 0);
}

static Typecheck check_enum_access(CheckContext *ctx, Node *node) {
    View enum_name = node->ast.enum_access.alias;
    View member_name = node->ast.enum_access.member;

    TypeEntry* entry = type_table_lookup(enum_name);
    if(!entry || entry->kind != TYPE_ENTRY_ENUM) {
        diag_error(ctx->ctx, node->filename, node->line, node->col, 
            "'%.*s' nao e um enum declarado", (int)enum_name.len, enum_name.start);
        return type_error();
    }

    for(size_t i = 0; i < entry->enumdef.member_count; ++i) {
        if(view_equals_view(entry->enumdef.members[i].name, member_name)) {
            node->ast.enum_access.iresolved_type = entry->enumdef.members[i].ivalue;

            Typecheck type = make_type(IDENTIFIER, 0);
            type.alias = enum_name;
            return type;
        }
    }

    diag_error(ctx->ctx, node->filename, node->line, node->col,
        "'%.*s' nao e um membro de '%.*s'", (int)member_name.len, member_name.start, (int)enum_name.len, enum_name.start);
    return type_error();
}

static void check_aggregate_decl(CheckContext *ctx, Node *node) {
    View name = node->ast.aggregate.name;

    if(node->ast.aggregate.members != NULL) {
        if(name.len > 0 && type_table_lookup(name)) {
            diag_error(ctx->ctx, node->filename, node->line, node->col, 
                "tipo '%.*s' ja declarado", (int)name.len, name.start);
        } else {
            TypeEntry* entry = NULL;
            if(name.len > 0) {
                entry = type_table_insert(name);
                entry->kind = TYPE_ENTRY_AGGREGATE;
            }

            AggregateDef* def = compute_aggregate_layout(ctx, node);
            if(entry) entry->aggregate = *def;
        }
    }

    if(node->ast.aggregate.tailing_decl) check_var_decl(ctx, node->ast.aggregate.tailing_decl);
}

static void check_newtype_decl(CheckContext *ctx, Node *node) {
    View name = node->ast.newtype.alias;

    if(newtype_table_lookup(name)) {
        diag_error(ctx->ctx, node->filename, node->line, node->col,
            "O tipo '%.*s' ja foi declarado anteriormente", (int)name.len, name.start);
        return;
    }

    Typecheck underlying = resolve_type_spec(ctx, node->ast.newtype.underlying, node);

    TypeEntry* entry = newtype_table_insert(name);
    entry->kind = TYPE_ENTRY_ALIAS;
    entry->alias = underlying;
}

static Typecheck check_literal(Node *node) {
    switch (node->ast.literal.kind)
    {
    case INT: return make_type(KINT32, 0);
    case BIG: return make_type(KBIG, 0);
    case UNSIGNED: return make_type(KUINT32, 0);
    case FLOAT: return make_type(KFLOAT, 0);
    case DOUBLE: return make_type(KDOUBLE, 0);
    case HEXA: return make_type(KHEXA, 0);
    case CHAR: return make_type(KCHAR, 0);
    case STRING: return make_type(KCHAR, 1);
    }

    return type_error();
}

static Typecheck check_var_access(CheckContext *ctx, Node *node) {
    Typecheck type = { 0 };
    if(!resolve_variable(ctx->current_scope, node->ast.call_variable.name, &type)) {
        diag_error(ctx->ctx, node->filename, node->line, node->col, 
            "A variavel '%.*s' nao foi declarada anteriormente", (int)node->ast.call_variable.name.len, node->ast.call_variable.name.start);
        return type_error();
    }

    return type;
}

static Typecheck resolve_type_spec(CheckContext *ctx, TypeSpec spec, Node *node) {
    if(spec.nested) {
        if(spec.base == KENUM) {
            Node* enum_node = spec.nested;

            if(enum_node->ast.enum_decl.members == NULL) {
                View enum_name = enum_node->ast.enum_decl.name;
                TypeEntry* entry = type_table_lookup(enum_name);
                if(!entry) return type_error();

                Typecheck type = make_type(IDENTIFIER, spec.ptr_lvl);
                type.alias = enum_name;
                apply_array_info(&type, spec);
                return type;
            }

            check_enum_decl(ctx, enum_node);
            Typecheck type = make_type(IDENTIFIER, spec.ptr_lvl);
            type.alias = enum_node->ast.enum_decl.name;
            apply_array_info(&type, spec);
            return type;
        }

        Node* aggregate_node = spec.nested;

        if(aggregate_node->ast.aggregate.members == NULL) {
            View aggregate_name = aggregate_node->ast.aggregate.name;
            TypeEntry* entry = type_table_lookup(aggregate_name);
            if(!entry) return type_error();

            Typecheck type = make_type(IDENTIFIER, spec.ptr_lvl);
            type.alias = aggregate_name;
            if(entry->kind == TYPE_ENTRY_AGGREGATE) type.inline_def = &entry->aggregate;
            apply_array_info(&type, spec);
            return type;
        }

        AggregateDef* def = compute_aggregate_layout(ctx, aggregate_node);
        Typecheck type = make_type(IDENTIFIER, spec.ptr_lvl);
        type.inline_def = def;

        if(aggregate_node->ast.aggregate.name.len > 0) {
            View nested_name = aggregate_node->ast.aggregate.name;
            TypeEntry* entry = type_table_insert(nested_name);
            
            if(entry) {
                entry->kind = TYPE_ENTRY_AGGREGATE;
                entry->aggregate = *def;
                type.alias = nested_name;
            } else {
                diag_error(ctx->ctx, node->filename, node->line, node->col, 
                    "O tipo '%.*s' ja foi declarado", (int)nested_name.len, nested_name.start);
            }
        }

        apply_array_info(&type, spec);
        return type;
    }

    if((spec.base == KDATA || spec.base == KENUM || spec.base == KCOPERATE) && spec.alias.len > 0) {
        TypeEntry* entry = type_table_lookup(spec.alias);
        if(!entry) return type_error();

        Typecheck type = make_type(IDENTIFIER, spec.ptr_lvl);
        type.alias = spec.alias;
        if(entry->kind == TYPE_ENTRY_AGGREGATE) type.inline_def = &entry->aggregate;

        apply_array_info(&type, spec);
        return type;
    }

    if(spec.base != IDENTIFIER) {
        Typecheck type = make_type(spec.base, spec.ptr_lvl);
        apply_array_info(&type, spec);
        return type;
    }

    TypeEntry* entry = type_table_lookup(spec.alias);
    if(!entry) return type_error();

    if(entry->kind == TYPE_ENTRY_ALIAS) {
        Typecheck type = entry->alias;
        type.ptr_lvl += spec.ptr_lvl;
        apply_array_info(&type, spec);
        return type;
    }

    Typecheck type = make_type(IDENTIFIER, spec.ptr_lvl);
    type.alias = spec.alias;

    if(entry->kind == TYPE_ENTRY_AGGREGATE) {
        type.inline_def = &entry->aggregate;
    }

    if(type.base == KLINK && type.ptr_lvl > 0) {
        diag_error(ctx->ctx, node->filename, node->line, node->col,
            "Variaveis de tipo link nao podem ser ponteiros/arrays");
    }

    apply_array_info(&type, spec);
    return type;
}

static Typecheck check_binary_op(CheckContext *ctx, Node *node) {
    Node* left = node->ast.binary.left;
    Node* right = node->ast.binary.right;

    Typecheck ltype = check_expr(ctx, left);
    Typecheck rtype = check_expr(ctx, right);
    TokenType op = node->ast.binary.op;

    if(type_is_error(ltype) || type_is_error(rtype)) return type_error();

    switch (op)
    {
    case OP_PLUS: case OP_MINUS: case OP_STAR: case OP_SLASH: case OP_MOD: {
        if(isptr(ltype) && isintegerbase(rtype.base) && !isptr(rtype)) 
        return ltype;
        if(op == OP_PLUS && isptr(rtype) && isintegerbase(ltype.base) && !isptr(ltype))
            return rtype;

        if(op == OP_MINUS && isptr(ltype) && isptr(rtype)) {
            if(!type_equals(ltype, rtype)) {
                diag_error(ctx->ctx, node->filename, node->line, node->col,
                    "subtracao entre ponteiros de tipos diferentes");
                return type_error();
            }
            return make_type(KINT64, 0);
        }

        if(isisland(ltype) || isisland(rtype)) {
            diag_error(ctx->ctx, node->filename, node->line, node->col,
                "Operadores aritmeticos exigem tipos numericos");
            return type_error();
        }

        if(op == OP_MOD && (isfloatbase(ltype.base) || isfloatbase(rtype.base))) {
            diag_error(ctx->ctx, node->filename, node->line, node->col,
                "'%%' exige operandos inteiros");
            return type_error();
        }

        return promote_from_arith(ltype, rtype);
    }

    case OP_LT: case OP_GT: case OP_LE: case OP_GE:
    case OP_BANGEQ: case OP_EQUALS: {
        if(isisland(ltype) != isisland(rtype)) {
            diag_error(ctx->ctx, node->filename, node->line, node->col,
                "Operandos incompativeis para a comparacao");
            return type_error();
        }

        if(isisland(ltype) && !type_equals(ltype, rtype) &&
            !(islink(ltype) && isptr(rtype)) && !(isptr(ltype) && islink(rtype))) {
                diag_error(ctx->ctx, node->filename, node->line, node->col,
                    "Comparacao entre tipos de endereco imcompativeis");
                return type_error();
            }

        return make_type(KUINT8, 0);
    }

    case OP_LOGAND: case OP_LOGOR: {
        return make_type(KUINT8, 0);
    }

    case OP_AND: case OP_OR: case OP_XOR: case OP_LSHIFT: case OP_RSHIFT: {
        if(!isintegerbase(ltype.base) || !isintegerbase(rtype.base)) {
            diag_error(ctx->ctx, node->filename, node->line, node->col,
                "Operando bit a bit exige tipos inteiros");
            return type_error();
        }

        return promote_from_arith(ltype, rtype);
    }

    case OP_ASSIGN: {
        if(!islvalue(left)) {
            diag_error(ctx->ctx, node->filename, node->line, node->col, 
                "Lado esquerdo da atibuicao precisa ser uma variavel");
            return type_error();
        }

        report_assignable(ctx, rtype, ltype, right, node);
        return ltype;
    }

    default: {
        if(isarith_compoundop(op)) {
            if(!islvalue(left)) {
                diag_error(ctx->ctx, node->filename, node->line, node->col,
                    "Lado esquerdo da atribuicao precisa ser uma variavel");
                return type_error();
            }

            if(isisland(ltype) || isisland(rtype)) {
                diag_error(ctx->ctx, node->filename, node->line, node->col,
                    "Operador aritimetico composto exige tipos numericos");
                return type_error();
            }

            if(op == OP_MODEQ && (isfloatbase(ltype.base) || isfloatbase(rtype.base))) {
                diag_error(ctx->ctx, node->filename, node->line, node->col,
                    "'%%=' exige operandos inteiros");
                return type_error();
            }

            Typecheck result = promote_from_arith(ltype, rtype);
            report_assignable(ctx, result, ltype, NULL, node);
            return ltype;
        }

        if(isbitwise_compoundop(op)) {
            if(!islvalue(left)) {
                diag_error(ctx->ctx, node->filename, node->line, node->col,
                    "Lado esquerdo da atribuicao precisa ser uma variavel");
                return type_error();
            }

            if(!isintegerbase(ltype.base) || !isintegerbase(rtype.base)) {
                diag_error(ctx->ctx, node->filename, node->line, node->col, 
                    "Operador bit a bit composto exige tipos inteiros");
                return type_error();
            }

            Typecheck result = promote_from_arith(ltype, rtype);
            report_assignable(ctx, result, ltype, NULL, node);
            return ltype;
        }

        diag_error(ctx->ctx, node->filename, node->line, node->col,
            "Operador desconhecido");
        return type_error();
    }
    }
}

static Typecheck check_array(CheckContext *ctx, Node *node) {
    Node* left = node->ast.binary.left;
    Node* right = node->ast.binary.right;

    Typecheck base = check_expr(ctx, left);
    Typecheck index = check_expr(ctx, right);

    if(type_is_error(base) || type_is_error(index)) return type_error();

    if(base.ptr_lvl == 0) {
        diag_error(ctx->ctx, node->filename, node->line, node->col,
            "indexacao '[]' so pode ser aplicada a um ponteiro ou array");
        return type_error();
    }

    if(index.ptr_lvl > 0 || !isintegerbase(index.base)) {
        diag_error(ctx->ctx, node->filename, node->line, node->col,
            "Indices de arrays precisa ser um valor inteiro");
        return type_error();
    }

    Typecheck type = base;
    type.ptr_lvl--;
    return type;
}

static Typecheck check_unary_op(CheckContext *ctx, Node *node) {
    Typecheck operand = check_expr(ctx, node->ast.unary.operand);
    if(type_is_error(operand)) return type_error();

    switch (node->ast.unary.op)
    {
    case OP_MINUS: {
        if(isisland(operand)) {
            diag_error(ctx->ctx, node->filename, node->line, node->col,
                "Operador unario '-' exige tipo numerico");
            return type_error();
        }

        return operand;
    }

    case OP_BANG: {
        return make_type(KUINT8, 0);
    }

    case OP_AND: {
        return make_type(operand.base, operand.ptr_lvl + 1);
    }

    case OP_STAR: {
        if(operand.ptr_lvl == 0) {
            diag_error(ctx->ctx, node->filename, node->line, node->col,
                "Nao e possivel deferenciar um valor que nao e ponteiro");
            return type_error();
        }

        return make_type(operand.base, operand.ptr_lvl - 1);
    }

    case OP_PLUS_PLUS: case OP_MINUS_MINUS: {
        if(isptr(operand)) return operand;

        if(isisland(operand)) {
            diag_error(ctx->ctx, node->filename, node->line, node->col,
                "Os operadores '++'/'--' exigem tipos numericos");
            return type_error();
        }

        return operand;
    }

    case OP_DESC: {
        if(!isintegerbase(operand.base) || isisland(operand)) {
            diag_error(ctx->ctx, node->filename, node->line, node->col,
                "Operador '~' exige um tipo numerico inteiro");
            return type_error();
        }

        return operand;
    }

    default:
        return operand;
    }
}

static Typecheck check_cast(CheckContext *ctx, Node *node) {
    check_expr(ctx, node->ast.cast.operand);

    Typecheck target = resolve_type_spec(ctx, node->ast.cast.spec, node);

    if(type_is_error(target)) {
        diag_error(ctx->ctx, node->filename, node->line, node->col,
            "Tipo invalido para cast");
        return type_error();
    }

    return target;
}

static Typecheck check_ternary(CheckContext *ctx, Node *node) {
    check_expr(ctx, node->ast.if_stmt.condition);

    Typecheck ttype = check_expr(ctx, node->ast.if_stmt.then);
    Typecheck otype = check_expr(ctx, node->ast.if_stmt.otherwise);

    if(type_is_error(ttype) || type_is_error(otype)) return type_error();
    if(type_equals(ttype, otype)) return ttype;

    const char* reason = NULL;

    if(check_assignable(otype, ttype, NULL, false, &reason) != COMPAT_ERROR) return ttype;
    if(check_assignable(ttype, otype, NULL, false, &reason) != COMPAT_ERROR) return otype;
    
    diag_error(ctx->ctx, node->filename, node->line, node->col,
        "Ramos do operador ternario tem tipos incompativeis");
    return type_error();
}

static Typecheck check_func_call(CheckContext *ctx, Node *node) {
    View name = node->ast.call_function.name;
    FuncEntry* entry = function_table_lookup(name);

    if(!entry) {
        diag_error(ctx->ctx, node->filename, node->line, node->col,
            "A funcao '%.*s' nao foi declarada nesse escopo", (int)name.len, name.start);
        return type_error();
    }

    FunctionSignature* signature = &entry->signature;
    NodeList* args = node->ast.call_function.args;

    if(args->count < signature->param_count || (!signature->variadic && args->count > signature->param_count)) {
        diag_error(ctx->ctx, node->filename, node->line, node->col, 
            "Numero incorreto de argumentos para '%.*s' esperado %zu, recebeu %zu", (int)name.len, name.start,
            signature->param_count, args->count);
        return signature->call_type;
    }

    for(size_t i = 0; i < signature->param_count; ++i) {
        if(view_equals_view(name, (View){ "__syscall_builtin", 17 })) break;
        if(view_equals_view(name, (View){ "__atomic", 8 })) break;
        Node* arg_node = args->items[i];
        Typecheck atype = check_expr(ctx, arg_node);
        report_assignable(ctx, atype, signature->param_types[i], arg_node, arg_node);
    }

    for(size_t i = signature->param_count; i < args->count; ++i) {
        check_expr(ctx, args->items[i]);
    }

    return signature->call_type;
}

static Typecheck check_field_access(CheckContext *ctx, Node *node) {
    check_expr(ctx, node->ast.field_access.base);

    Node* base = node->ast.field_access.base;
    Typecheck type = base->resolved_type;
    bool arrow = node->ast.field_access.arrow;

    if(arrow && type.ptr_lvl == 0) {
        diag_error(ctx->ctx, node->filename, node->line, node->col,
            "O operador '->' so pode ser usado por ponteiros");
        return type_error();
    }

    if(!arrow && type.ptr_lvl > 0) {
        diag_error(ctx->ctx, node->filename, node->line, node->col,
            "O operador '.' so pode ser usado quando nao houver ponteiro");
        return type_error();
    }

    AggregateDef* def = type.inline_def;
    if(!def) {
        diag_error(ctx->ctx, node->filename, node->line, node->col,
            "Tipo base nao e data/coperate");
        return type_error();
    }

    FieldInfo* field = find_field(def, node->ast.field_access.name);
    if(!field) {
        diag_error(ctx->ctx, node->filename, node->line, node->col,
            "campo '%.*s' nao existe no tipo",
            (int)node->ast.field_access.name.len,
            node->ast.field_access.name.start);
        return type_error();
    }

    node->ast.field_access.offset = field->offset;
    node->ast.field_access.base_type = type;
    return field->type;
}

static Typecheck check_expr(CheckContext *ctx, Node *node) {
    if(!node) return type_error();

    Typecheck type = { 0 };
    switch (node->kind)
    {
    case NODE_LITERAL: type = check_literal(node); break;
    case NODE_VAR_CALL: type = check_var_access(ctx, node); break;
    case NODE_BINARY_OP: type = check_binary_op(ctx, node); break;
    case NODE_UNARY_OP: type = check_unary_op(ctx, node); break;
    case NODE_POSTFIX_OP: type = check_unary_op(ctx, node); break;
    case NODE_FUNC_CALL: type = check_func_call(ctx, node); break;
    case NODE_ENUM_CALL: type = check_enum_access(ctx, node); break;
    case NODE_FIELD_CALL: type = check_field_access(ctx, node); break;
    case NODE_ARRAY: type = check_array(ctx, node); break;
    case NODE_CAST: type = check_cast(ctx, node); break;
    case NODE_TERNARY: type = check_ternary(ctx, node); break;
    case NODE_BYTES: type = check_bytes(ctx, node); break;
    default:
        diag_error(ctx->ctx, node->filename, node->line, node->col,
            "Expressao nao suportada pelo typechecker");
        return type_error();
    }

    node->resolved_type = type;
    return type;
}

static void check_statement(CheckContext *ctx, Node *node);

static void check_enum_decl(CheckContext *ctx, Node *node) {
    View name = node->ast.enum_decl.name;

    if(name.len > 0 && type_table_lookup(name)) {
        diag_error(ctx->ctx, node->filename, node->line, node->col,
            "O tipo '%.*s' ja foi declarado anteriormente", (int)name.len, name.start);
        return;
    }

    NodeList* members = node->ast.enum_decl.members;
    EnumMemberInfo* infos = (EnumMemberInfo*)arena_alloc(ctx->arena, sizeof(EnumMemberInfo) * members->count);

    unsigned long long inext = 0;

    for(size_t i = 0; i < members->count; ++i) {
        Node* member = members->items[i];

        if(member->ast.enum_member.value && member->ast.enum_member.value != NODE_UNDEFINED) {
            Node* value = member->ast.enum_member.value;

            if((value->kind != NODE_LITERAL && value->kind != NODE_BINARY_OP) || (value->ast.literal.kind != INT
                && value->ast.literal.kind != UNSIGNED)) {
                fprintf(stdout, "Literal_kind: %s", literal_to_str(value->ast.literal.kind));
                diag_error(ctx->ctx, value->filename, value->line, value->col,
                    "Valor de membro de enum precisa ser umas constante inteira literal");
                inext = 0;
            }
            else inext = (value->ast.literal.kind != UNSIGNED && value->ast.literal.kind != HEXA) ? (size_t)value->ast.literal.integer64 : value->ast.literal.unsigned64;
        }

        infos[i].name = member->ast.enum_member.name;
        infos[i].ivalue = inext;

        inext++;
    }

    if(name.len > 0) {
        TypeEntry* entry = type_table_insert(name);
        entry->kind = TYPE_ENTRY_ENUM;
        entry->enumdef.member_count = members->count;
        entry->enumdef.members = infos;
    }
}

static void check_while_loop(CheckContext *ctx, Node *node) {
    check_expr(ctx, node->ast.while_loop.condition);

    bool prev_in_loop = ctx->in_loop;
    ctx->in_loop = true;

    check_statement(ctx, node->ast.while_loop.body);

    ctx->in_loop = prev_in_loop;
}

static void check_do_while_loop(CheckContext *ctx, Node *node) {
    bool prev_in_loop = ctx->in_loop;
    ctx->in_loop = true;

    check_statement(ctx, node->ast.while_loop.body);

    ctx->in_loop = prev_in_loop;

    check_expr(ctx, node->ast.while_loop.condition);
}

static void check_for_loop(CheckContext *ctx, Node *node) {
    ctx_scope_push(ctx);

    Node* init = node->ast.for_loop.init;
    if(init) {
        if(init->kind == NODE_VAR_DECL) check_var_decl(ctx, init);
        else check_expr(ctx, init);
    }

    if(node->ast.for_loop.condition) check_expr(ctx, node->ast.for_loop.condition);
    if(node->ast.for_loop.increment) check_expr(ctx, node->ast.for_loop.increment);

    bool prev_in_loop = ctx->in_loop;
    ctx->in_loop = true;

    check_statement(ctx, node->ast.for_loop.body);

    ctx->in_loop = prev_in_loop;

    ctx_scope_pop(ctx);
}

static void check_break_stmt(CheckContext *ctx, Node *node) {
    if(!ctx->in_loop) {
        diag_error(ctx->ctx, node->filename, node->line, node->col,
            "O 'break' pode ser usado somente em loops");
    }
}

static void check_continue_stmt(CheckContext *ctx, Node *node) {
    if(!ctx->in_loop) {
        diag_error(ctx->ctx, node->filename, node->line, node->col,
            "O 'continue' pode ser usado somente em loops");
    }
}

static void check_jump_label(Node *node) {
    View name = node->ast.jmp_label.name;
    label_reference(name, node);
}

static void check_load_label(CheckContext *ctx, Node *node) {
    View name = node->ast.jmp_label.name;
    LabelEntry* entry = label_declare(name, node);
    if(!entry) {
        diag_error(ctx->ctx, node->filename, node->line, node->col, 
            "label '%.*s' ja foi definido no arquivo", (int)name.len, name.start);
    }
}

static void check_var_decl(CheckContext *ctx, Node *node) {
    View name = node->ast.decl_variable.name;

    TypeSpec* spec = &node->ast.decl_variable.call_type;

    if(spec->is_array) {
        NodeList* dimentions = spec->array_dimentions;
    
        for(size_t i = 0; i < dimentions->count; ++i) {
            Node* dimention = dimentions->items[i];

            if(dimention_isommited(dimention)) {
                if(i != 0) {
                    diag_error(ctx->ctx, node->filename, node->line, node->col,
                        "Somente a primeira dimensao do array pode ser omitida");
                }
                continue;
            }

            Typecheck dimtype = check_expr(ctx, dimention);
            if(!isintegerbase(dimtype.base)) {
                diag_error(ctx->ctx, node->filename, node->line, node->col,
                    "O tamanho do array deve ser inteiro");
            }
        }

        if(typespec_has_vla(spec)) {
            if(node->ast.decl_variable._static) {
                diag_error(ctx->ctx, node->filename, node->line, node->col,
                    "Array de tamanho dinamico nao pode ser static");
            }

            if(!ctx->in_function) {
                diag_error(ctx->ctx, node->filename, node->line, node->col,
                    "Array de tamanho dinamico so podem existir dentro de funcoes");
            }
        }
    }

    Typecheck declared = resolve_type_spec(ctx, node->ast.decl_variable.call_type, node);

    if(type_is_error(declared)) {
        diag_error(ctx->ctx, node->filename, node->line, node->col,
            "Tipo nao declarado para '%.*s'", (int)name.len, name.start);
    }

    if(node->ast.decl_variable.init) {
        Typecheck current = check_expr(ctx, node->ast.decl_variable.init);
        report_assignable(ctx, current, declared, node->ast.decl_variable.init, node);
    }

    if(node->ast.decl_variable._atomic) {
        bool valid = isintegerbase(declared.base) || isptr(declared) || islink(declared);
        size_t size = type_size(declared);

        if(!valid || size > 8) {
            diag_error(ctx->ctx, node->filename, node->line, node->col,
                "Qualificado atomic so e valido em inteiros ou ponteiros de ate 8 bytes");
        } else {
            declared._atomic = true;
        }
    }

    node->resolved_type = declared;

    if(!ctx->in_function) {
        GlobalEntry* global = global_var_decl(name, declared, node, node->ast.decl_variable._static, node->ast.decl_variable._extern, 
            node->ast.decl_variable.init != NULL);

        if(!global) {
            diag_error(ctx->ctx, node->filename, node->line, node->col, 
                "Variavel global '%.*s' ja declarada", (int)name.len, name.start);
        }
    }

    if(!ctx_scope_declare(ctx, name, declared)) {
        diag_error(ctx->ctx, node->filename, node->line, node->col,
            "A variavel '%.*s' ja foi declarada nesse escopo", (int)name.len, name.start);
    }
}

static void check_if_stmt(CheckContext *ctx, Node *node) {
    check_expr(ctx, node->ast.if_stmt.condition);
    check_statement(ctx, node->ast.if_stmt.then);

    if(node->ast.if_stmt.otherwise) check_statement(ctx, node->ast.if_stmt.otherwise);
}

static void check_block_stmt(CheckContext *ctx, Node *node) {
    ctx_scope_push(ctx);

    NodeList* statements = node->ast.program.statements;

    for(size_t i = 0; i < statements->count; ++i) {
        check_statement(ctx, statements->items[i]);
    }

    ctx_scope_pop(ctx);
}

static void check_func_decl(CheckContext *ctx, Node *node) {
    if(ctx->in_function) {
        diag_error(ctx->ctx, node->filename, node->line, node->col,
            "Funcoes aninhadas nao sao suportadas pela linguagem");
        return;
    }

    View name = node->ast.decl_function.name;
    bool has_body = (node->ast.decl_function.body != NULL);

    Typecheck call_type = resolve_type_spec(ctx, node->ast.decl_function.call_type, node);

    if(type_is_error(call_type)) {
        diag_error(ctx->ctx, node->filename, node->line, node->col,
            "Tipo de retorno nao declarado para '%.*s'", (int)name.len, name.start);
    }

    NodeList* params = node->ast.decl_function.params;
    Typecheck* ptype = (Typecheck*)arena_alloc(ctx->arena, sizeof(Typecheck) * params->count);

    for(size_t i = 0; i < params->count; ++i) {
        Node* param = params->items[i];
        ptype[i] = resolve_type_spec(ctx, param->ast.decl_variable.call_type, node);
        param->resolved_type = ptype[i];
    }

    FunctionSignature signature;
    signature.call_type = call_type;
    signature.param_types = ptype;
    signature.param_count = params->count;
    signature.variadic = node->ast.decl_function._variadic;

    FuncEntry* exist = function_table_lookup(name);

    if(exist) {
        if(exist->has_body) {
            diag_error(ctx->ctx, node->filename, node->line, node->col,
                "A funcao '%.*s' ja foi declarada antes", (int)name.len, name.start);
            return;
        }

        if(!signature_equal(&exist->signature, &signature)) {
            diag_error(ctx->ctx, node->filename, node->line, node->col,
                "Definicao de '%.*s' nao corresponde ao prototipo declarado anteriormente", (int)name.len, name.start);
            return;
        }

        exist->signature = signature;
        exist->decl_node = node;
        if(has_body) {
            exist->has_body = true;
            exist->_pending = false;
        }
    } else {
        FuncEntry* entry = function_table_insert(name);
        entry->signature = signature;
        entry->_static = node->ast.decl_function._static;
        entry->_extern = node->ast.decl_function._extern;
        entry->_pending = !has_body && !node->ast.decl_function._extern;
        entry->has_body = has_body;
        entry->decl_node = node;
    }

    if(!has_body) return;

    ctx_scope_push(ctx);

    for(size_t i = 0; i < params->count; ++i) {
        Node* param = params->items[i];
        ctx_scope_declare(ctx, param->ast.decl_variable.name, ptype[i]);
    }

    ctx->current_call_type = call_type;
    ctx->in_function = true;

    check_statement(ctx, node->ast.decl_function.body);

    ctx->in_function = false;

    if(view_equals_view(name, (View){ "start", 5 })) {
        if(ctx->entry_function != NULL) {
            diag_error(ctx->ctx, node->filename, node->line, node->col,
                "Multiplas definicoes do entry point 'start'");
        }
        else ctx->entry_function = node;
    }

    ctx_scope_pop(ctx);
}

static void check_call_stmt(CheckContext *ctx, Node *node) {
    if(!ctx->in_function) {
        diag_error(ctx->ctx, node->filename, node->line, node->col,
            "'call' fora do escopo de uma funcao");
        return;
    }

    bool call_void = (ctx->current_call_type.base == KVOID && ctx->current_call_type.ptr_lvl == 0);

    if(node->ast.call_stmt.value) {
        if(call_void) {
            diag_error(ctx->ctx, node->filename, node->line, node->col, 
                "Funcoes com retorno 'void' nao podem retornar um valor");
            return;
        }

        Typecheck type = check_expr(ctx, node->ast.call_stmt.value);
        report_assignable(ctx, type, ctx->current_call_type, node->ast.call_stmt.value, node);
    } else if(!call_void) {
        diag_error(ctx->ctx, node->filename, node->line, node->col,
            "A funcao esperado um valor de retorno");
    }
}

static void check_statement(CheckContext *ctx, Node *node) {
    if(!node) return;

    switch (node->kind)
    {
    case NODE_VAR_DECL: check_var_decl(ctx, node); break;
    case NODE_IF_STMT: check_if_stmt(ctx, node); break;
    case NODE_BLOCK_STMT: check_block_stmt(ctx, node); break;
    case NODE_FUNC_DECL: check_func_decl(ctx, node); break;
    case NODE_BREAK_STMT: check_break_stmt(ctx, node); break;
    case NODE_CONTINUE_STMT: check_continue_stmt(ctx, node); break;
    case NODE_FOR_LOOP: check_for_loop(ctx, node); break;
    case NODE_WHILE_LOOP: check_while_loop(ctx, node); break;
    case NODE_DO_WHILE_LOOP: check_do_while_loop(ctx, node); break;
    case NODE_FUNC_CALL: check_func_call(ctx, node); break;
    case NODE_DATA:
    case NODE_COPERATE: check_aggregate_decl(ctx, node); break;
    case NODE_ENUM: check_enum_decl(ctx, node); break;
    case NODE_NEWTYPE: check_newtype_decl(ctx, node); break;
    case NODE_CALL_STMT: check_call_stmt(ctx, node); break;
    case NODE_JUMP_LABEL: check_jump_label(node); break;
    case NODE_LOAD_LABEL: check_load_label(ctx, node); break;
    case NODE_UNARY_OP:
    case NODE_BINARY_OP:
    case NODE_POSTFIX_OP:
    case NODE_BYTES:
        check_expr(ctx, node);
        break;
    case NODE_UNDEFINED: diag_error(ctx->ctx, node->filename, node->line, node->col,
        "A expressao nao faz parte do sistema"); break;
    default:
        break;
    }
}

static void register_builtins(CheckContext *ctx) {
    View syscall_builtin = { "__syscall_builtin", 17 };

    Typecheck* params_syscall = (Typecheck*)arena_alloc(ctx->arena, sizeof(Typecheck) * 7);

    for(int i = 0; i < 7; ++i) {
        params_syscall[i].base = KINT64;
        params_syscall[i].ptr_lvl = 0;
    }

    Typecheck call_type_syscall = { 0 };
    call_type_syscall.base = KINT64;
    call_type_syscall.ptr_lvl = 0;

    FunctionSignature syscall_signature = { 0 };
    syscall_signature.call_type = call_type_syscall;
    syscall_signature.param_count = 7;
    syscall_signature.param_types = params_syscall;
    syscall_signature.variadic = false;

    FuncEntry* syscall_entry = function_table_insert(syscall_builtin);
    syscall_entry->signature = syscall_signature;
    syscall_entry->signature = syscall_signature;
    syscall_entry->_static = false;
    syscall_entry->_extern = false;
    syscall_entry->_pending = false;
    syscall_entry->has_body = false;
    syscall_entry->decl_node = NULL;

    View atomic_built = { "__atomic", 8 };

    Typecheck* params_atomic = (Typecheck*)arena_alloc(ctx->arena, sizeof(Typecheck) * 4);

    for(int i = 0; i < 4; ++i) {
        params_atomic[i].base = KINT64;
        params_atomic[i].ptr_lvl = 0;
    }

    Typecheck call_type_atomic = { 0 };
    call_type_atomic.base = KINT64;
    call_type_atomic.ptr_lvl = 0;

    FunctionSignature atomic_signature = { 0 };
    atomic_signature.call_type = call_type_atomic;
    atomic_signature.param_count = 4;
    atomic_signature.param_types = params_atomic;
    atomic_signature.variadic = false;

    FuncEntry* atomic_entry = function_table_insert(atomic_built);
    atomic_entry->signature = atomic_signature;
    atomic_entry->_static = false;
    atomic_entry->_extern = false;
    atomic_entry->_pending = false;
    atomic_entry->has_body = false;
    atomic_entry->decl_node = NULL;
}

static void report_undefined_label(LabelEntry *entry, void *userdata) {
    CheckContext *ctx = (CheckContext*)userdata;
    if(!entry->_defined) {
        Node* node = entry->first_jump;
        diag_error(ctx->ctx, node->filename, node->line, node->col,
            "label '%.*s' usado em 'jump' mas nunca definido com 'load'",
            (int)entry->name.len, entry->name.start);
    }
}

static void check_unresolved_labels(CheckContext *ctx) {
    label_table_foreach(report_undefined_label, ctx);
}

void check_program(CheckContext *ctx, Node *program) {
    typecheck_globals_init(ctx->arena);
    ctx->current_call_type = make_type(KVOID, 0);

    register_builtins(ctx);

    NodeList* statements = program->ast.program.statements;

    for(size_t i = 0; i < statements->count; ++i) {
        check_statement(ctx, statements->items[i]);
    }

    check_unresolved_labels(ctx);
}
