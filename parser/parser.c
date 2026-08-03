#include "parser.h"

static Node* parse_expr(Parser *parser);
static Node* parse_block_stmt(Parser *parser);
static Node* parse_decl(Parser *parser);
static Node* parse_statement(Parser *parser);
static Node* parse_unary(Parser *parser);
static Node* parse_field_decl(Parser *parser);

Parser create_parser(Lexer *lexer, Arena *arena, DiagContext *context) {
    Parser parser;
    parser.arena = arena;
    parser.lexer = lexer;
    parser.current = next_token(lexer);
    parser.context = context;
    return parser;
}

static NodeList* list_create(Arena *arena) {
    NodeList* list = (NodeList*)arena_alloc(arena, sizeof(NodeList));
    list->capacity = 4;
    list->count = 0;
    list->items = (Node**)arena_alloc(arena, sizeof(Node*) * list->capacity);
    return list;
}

static void list_push(Arena *arena, NodeList *list, Node *item) {
    if(list->count >= list->capacity) {
        size_t new_cap = list->capacity * 2;
        Node** new_items = (Node**)arena_alloc(arena, sizeof(Node*) * new_cap);

        memcpy(new_items, list->items, list->count * sizeof(Node*));

        list->capacity = new_cap;
        list->items = new_items;
    }

    list->items[list->count++] = item;
}

static Token peek(Parser *parser) {
    return parser->current;
}

static Token peekprev(Parser *parser) {
    return parser->previous;
}

static Token advance(Parser *parser) {
    parser->previous = parser->current;
    parser->current = next_token(parser->lexer);
    return parser->previous;
}

static Token expected(Parser *parser, TokenType type, const char* fallback) {
    if(peek(parser).type != type) {
        diag_error(parser->context, parser->current.filename, parser->current.line, parser->current.col,
            "%s", fallback);
        return parser->current;
    }

    return advance(parser);
}

static bool match(Parser *parser, TokenType type) {
    if(peek(parser).type == type) {
        advance(parser);
        return true;
    }

    return false;
}

static bool istype(TokenType type) {
    return (type >= KVOID && type <= KINT64);
}

static TypeSpec type_spec_from_node(Node *type) {
    TypeSpec spec = { 0 };

    if(type->kind == NODE_TYPEREF) {
        if(type->ast.type_ref.primitive != 0) {
            spec.base = type->ast.type_ref.primitive;
            spec.name = (View){ 0 };
        } else {
            spec.base = IDENTIFIER;
            spec.name = type->ast.type_ref.name;
        }
        spec.nested = NULL;
    } else {
        spec.base = IDENTIFIER;
        spec.name = type->ast.aggregate.name;
        spec.nested = type;
    }

    return spec;
}

static Node* create_node_at(Parser *parser, NodeKind type, Token origin) {
    Node* node = (Node*)arena_alloc(parser->arena, sizeof(Node));
    memset(node, 0, sizeof(Node));
    node->line = origin.line;
    node->col = origin.col;
    node->kind = type;
    node->filename = origin.filename;
    return node;
}

static Node* create_node(Parser *parser, NodeKind type) {
    return create_node_at(parser, type, peek(parser));
}

static long long converthexa(Parser *parser, View hexa_string) {
    long long result = 0;

    for(size_t i = 2; i < hexa_string.len; ++i) {
        char c = hexa_string.start[i];
        int digit = 0;
        if(isnumeric(c)) digit = c - '0';
        else if(c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else {
            diag_warning(parser->context, peek(parser).filename, peek(parser).line, peek(parser).col,
                "Erro ao converter o hexadecimal");
            return result;
        }

        result = result * 16 + digit;
    }

    return result;
}

static long long convertint(Parser *parser, View int_string) {
    long long result = 0;
    for(size_t i = 0; i < int_string.len; ++i) {
        char c = int_string.start[i];
        if(isnumeric(c)) result = result * 10 + (c - '0');
        else {
            diag_warning(parser->context, parser->current.filename, parser->current.line, parser->current.col,
                "Erro ao criar o numero inteiro");
            return result;
        }
    }

    return result;
}

static double convertdouble(Parser *parser, View double_string) {
    double result = 0;
    bool pass_dot = false;
    long long divisor = 10;
    
    for(size_t i = 0; i < double_string.len; ++i) {
        char c = double_string.start[i];
        if(c == '.') {
            pass_dot = true;
            continue;
        }

        if(c == 'f') break;
        if(isnumeric(c) && !pass_dot) result = result * 10 + (c - '0');
        else if(isnumeric(c) && pass_dot) {
            result = result + (double)(c - '0') / divisor;
            divisor *= 10;
        }
        else {
            diag_warning(parser->context, parser->current.filename, parser->current.line, parser->current.col,
                "Erro ao criar o numero flutuante");
            return result;
        }
    }

    return result;
}

static Node* parse_primary(Parser *parser) {
    if(match(parser, INT_LIT)) {
        Node* node = create_node_at(parser, NODE_LITERAL, peekprev(parser));
        node->ast.literals.type = INT;
        node->ast.literals.integer64 = convertint(parser, peekprev(parser).value);
        return node;
    }

    if(match(parser, HEXA_LIT)) {
        Node* node = create_node_at(parser, NODE_LITERAL, peekprev(parser));
        node->ast.literals.type = HEXA;
        node->ast.literals.integer64 = converthexa(parser, peekprev(parser).value);
        return node;
    }

    if(match(parser, DOUBLE_LIT) || match(parser, FLOAT_LIT)) {
        Node* node = create_node_at(parser, NODE_LITERAL, peekprev(parser));
        node->ast.literals.type = (parser->previous.type == DOUBLE_LIT) ? DOUBLE : FLOAT;
        node->ast.literals.double64 = convertdouble(parser, peekprev(parser).value);
        return node;
    }

    if(match(parser, CHAR_LIT)) {
        Node* node = create_node_at(parser, NODE_LITERAL, peekprev(parser));
        node->ast.literals.type = CHAR;
        node->ast.literals.character = peekprev(parser).value;
        return node;
    }

    if(match(parser, STRING_LIT)) {
        Node* node = create_node_at(parser, NODE_LITERAL, peekprev(parser));
        node->ast.literals.type = STRING;
        node->ast.literals.string = peekprev(parser).value;
        return node;
    }

    if(match(parser, IDENTIFIER)) {
        Token name = peekprev(parser);

        if(peek(parser).type == COLON) {
            advance(parser);
            expected(parser, OP_LT, "Esperado '<' apos ':' no acesso de enum");
            expected(parser, IDENTIFIER, "Esperado nome do membro do enum");

            Node* node = create_node_at(parser, NODE_ENUM_ACCESS, name);
            node->ast.enum_access.enum_name = name.value;
            node->ast.enum_access.member_name = peekprev(parser).value;

            return node;
        }

        Node* node = create_node_at(parser, NODE_VAR_ACCESS, name);
        node->ast.access_variable.name = peekprev(parser).value;
        return node;
    }

    if(match(parser, OPEN_PAREN)) {
        Node* node = parse_expr(parser);
        expected(parser, CLOSE_PAREN, "Esperado ')' apos a expressao");
        return node;
    }

    return NULL;
}

static Node* parse_postfix(Parser *parser) {
    Node* expr = parse_primary(parser);

    while(1) {
        if(match(parser, OP_MINUS_MINUS) || match(parser, OP_PLUS_PLUS)) {
            TokenType op = peekprev(parser).type;
            Node* node = create_node_at(parser, NODE_POSTFIX_OP, peekprev(parser));
            node->ast.unary_operator.op = op;
            node->ast.unary_operator.operand = expr;
            expr = node;
            continue;
        }

        if(match(parser, OPEN_BRACKET)) {
            Node* node = create_node_at(parser, NODE_ARRAY, peekprev(parser));
            node->ast.binary_operator.left = expr;
            node->ast.binary_operator.right = parse_expr(parser);
            expected(parser, CLOSE_BRACKET, "Esperava ']' apos a expressao");
            expr = node;
            continue;
        }

        if(match(parser, DOT) || match(parser, OP_ARROW)) {
            bool arrow = peekprev(parser).type == OP_ARROW;
            expected(parser, IDENTIFIER, "Esperava nome do campo apos '.' ou '->'");

            Node* node = create_node_at(parser, NODE_FIELD_ACCESS, peekprev(parser));
            node->ast.field_access.arrow = arrow;
            node->ast.field_access.base = expr;
            node->ast.field_access.field_name = peekprev(parser).value;
            node->ast.field_access.field_offset = 0;
            expr = node;
            continue;
        }

        if(match(parser, OPEN_PAREN)) {
            if(expr->kind != NODE_VAR_ACCESS) {
                diag_error(parser->context, peekprev(parser).filename, peekprev(parser).line, peekprev(parser).col,
                    "Apenas identificadores podem ser chamados como funcao");
            }

            Node* node = create_node_at(parser, NODE_FUNC_CALL, peekprev(parser));
            node->ast.call_function.name = expr->ast.access_variable.name;
            node->ast.call_function.args = list_create(parser->arena);

            if(peek(parser).type != CLOSE_PAREN) {
                do {
                    Node* arg = parse_expr(parser);
                    list_push(parser->arena, node->ast.call_function.args, arg);
                } while(match(parser, COMMA));
            }

            expected(parser, CLOSE_PAREN, "Esperado ')' apos os argumentos");
            expr = node;
            continue;
        }

        break;
    }

    return expr;
}

static Node* parse_unary(Parser *parser) {
    if(match(parser, OP_MINUS) || match(parser, OP_MINUS_MINUS) || match(parser, OP_PLUS_PLUS) ||
        match(parser, OP_AND) || match(parser, OP_BANG) || match(parser, OP_STAR)) {
        
        TokenType op = peekprev(parser).type;
        Node* node = create_node_at(parser, NODE_UNARY_OP, peekprev(parser));
        node->ast.unary_operator.op = op;
        node->ast.unary_operator.operand = parse_unary(parser);
        return node;
    }

    return parse_postfix(parser);
}

static Node* parse_factor(Parser *parser) {
    Node* expr = parse_unary(parser);

    while(match(parser, OP_STAR) || match(parser, OP_SLASH) || match(parser, OP_MOD)) {
        TokenType op = peekprev(parser).type;
        Node* node = create_node_at(parser, NODE_BINARY_OP, peekprev(parser));
        node->ast.binary_operator.op = op;
        node->ast.binary_operator.left = expr;
        node->ast.binary_operator.right = parse_unary(parser);
        expr = node;
    }

    return expr;
}

static Node* parse_term(Parser *parser) {
    Node* expr = parse_factor(parser);

    while(match(parser, OP_PLUS) || match(parser, OP_MINUS)) {
        TokenType op = peekprev(parser).type;
        Node* node = create_node_at(parser, NODE_BINARY_OP, peekprev(parser));
        node->ast.binary_operator.op = op;
        node->ast.binary_operator.left = expr;
        node->ast.binary_operator.right = parse_factor(parser);
        expr = node;
    }

    return expr;
}

static Node* parse_shift(Parser *parser) {
    Node* expr = parse_term(parser);

    while(match(parser, OP_LSHIFT) || match(parser, OP_RSHIFT)) {
        TokenType op = peekprev(parser).type;
        Node* node = create_node_at(parser, NODE_BINARY_OP, peekprev(parser));
        node->ast.binary_operator.op = op;
        node->ast.binary_operator.left = expr;
        node->ast.binary_operator.right = parse_term(parser);
        expr = node;
    }

    return expr;
}

static Node* parse_comparison(Parser *parser) {
    Node* expr = parse_shift(parser);

    while(match(parser, OP_LT) || match(parser, OP_GT) || match(parser, OP_LE) || match(parser, OP_GE)) {
        TokenType op = peekprev(parser).type;
        Node* node = create_node_at(parser, NODE_BINARY_OP, peekprev(parser));
        node->ast.binary_operator.op = op;
        node->ast.binary_operator.left = expr;
        node->ast.binary_operator.right = parse_shift(parser);
        expr = node;
    }

    return expr;
}

static Node* parse_equality(Parser *parser) {
    Node* expr = parse_comparison(parser);

    while(match(parser, OP_EQUALS) || match(parser, OP_BANGEQ)) {
        TokenType op = peekprev(parser).type;
        Node* node = create_node_at(parser, NODE_BINARY_OP, peekprev(parser));
        node->ast.binary_operator.op = op;
        node->ast.binary_operator.left = expr;
        node->ast.binary_operator.right = parse_comparison(parser);
        expr = node;
    }

    return expr;
}

static Node* parse_bitwise_and(Parser *parser) {
    Node* expr = parse_equality(parser);

    while(match(parser, OP_AND)) {
        TokenType op = peekprev(parser).type;
        Node* node = create_node_at(parser, NODE_BINARY_OP, peekprev(parser));
        node->ast.binary_operator.op = op;
        node->ast.binary_operator.left = expr;
        node->ast.binary_operator.right = parse_equality(parser);
        expr = node;
    }

    return expr;
}

static Node* parse_bitwise_xor(Parser *parser) {
    Node* expr = parse_bitwise_and(parser);

    while(match(parser, OP_XOR)) {
        TokenType op = peekprev(parser).type;
        Node* node = create_node_at(parser, NODE_BINARY_OP, peekprev(parser));
        node->ast.binary_operator.op = op;
        node->ast.binary_operator.left = expr;
        node->ast.binary_operator.right = parse_bitwise_and(parser);
        expr = node;
    }

    return expr;
}

static Node* parse_bitwise_or(Parser *parser) {
    Node* expr = parse_bitwise_xor(parser);

    while(match(parser, OP_OR)) {
        TokenType op = peekprev(parser).type;
        Node* node = create_node_at(parser, NODE_BINARY_OP, peekprev(parser));
        node->ast.binary_operator.op = op;
        node->ast.binary_operator.left = expr;
        node->ast.binary_operator.right = parse_bitwise_xor(parser);
        expr = node;
    }

    return expr;
}

static Node* parse_assignment(Parser *parser) {
    Node* expr = parse_bitwise_or(parser);

    while(match(parser, OP_ASSIGN) || match(parser, OP_PLUSEQ) || match(parser, OP_MINUSEQ) || match(parser, OP_LSHIFTEQ) || 
        match(parser, OP_RSHIFTEQ) || match(parser, OP_STAREQ) || match(parser, OP_ANDEQ) || match(parser, OP_OREQ) ||
        match(parser, OP_MODEQ) || match(parser, OP_SLASHEQ) || match(parser, OP_XOREQ)) {
        
        TokenType op = peekprev(parser).type;
        Node* node = create_node_at(parser, NODE_BINARY_OP, peekprev(parser));
        node->ast.binary_operator.op = op;
        node->ast.binary_operator.left = expr;
        node->ast.binary_operator.right = parse_bitwise_or(parser);
        expr = node;
    }

    return expr;
}

static Node* parse_expr(Parser *parser) {
    return parse_assignment(parser);
}

static Node* parse_aggregate_decl_type_only(Parser *parser, TokenType keyword, NodeKind kind, const char* keyword_name) {
    Token start = peek(parser);
    expected(parser, keyword, keyword_name);

    View tag = {0};
    bool has_tag = false;

    if(peek(parser).type == IDENTIFIER) {
        advance(parser);
        tag = peekprev(parser).value;
        has_tag = true;
    }

    Node* node = create_node_at(parser, kind, start);
    node->ast.aggregate.name = tag;
    node->ast.aggregate.members = NULL;
    node->ast.aggregate.tailing_decl = NULL;

    if(peek(parser).type == OPEN_BRACE) {
        node->ast.aggregate.members = list_create(parser->arena);
        expected(parser, OPEN_BRACE, "Esperado '{'");
        while(peek(parser).type != CLOSE_BRACE && peek(parser).type != EOFF) {
            Node* field = parse_field_decl(parser);
            list_push(parser->arena, node->ast.aggregate.members, field);
        }
        expected(parser, CLOSE_BRACE, "Esperado '}'");
    } else if(!has_tag) {
        diag_error(parser->context, peek(parser).filename, peek(parser).line, peek(parser).col,
            "Esperado '{' ou nome apos '%s'", keyword_name);
    }

    return node;
}

static Node* parse_type_specifier(Parser *parser) {
    Token start = peek(parser);

    if(peek(parser).type == KDATA) 
        return parse_aggregate_decl_type_only(parser, KDATA, NODE_DATA, "data");
    if(peek(parser).type == KCOPERATE)
        return parse_aggregate_decl_type_only(parser, KCOPERATE, NODE_COPERATE, "coperate");

    if(istype(peek(parser).type)) {
        advance(parser);
        Node* node = create_node_at(parser, NODE_TYPEREF, start);
        node->ast.type_ref.primitive = peekprev(parser).type;
        node->ast.type_ref.name = (View){ 0 };
        return node;
    }

    if(peek(parser).type == IDENTIFIER) {
        advance(parser);
        Node* node = create_node_at(parser, NODE_TYPEREF, start);
        node->ast.type_ref.primitive = 0;
        node->ast.type_ref.name = peekprev(parser).value;
        return node;
    }

    diag_error(parser->context, peek(parser).filename, peek(parser).line, peek(parser).col,
        "Esperado especificador de tipo");
    return NULL;
}

static Node* parse_field_decl(Parser *parser) {
    Node* type = parse_type_specifier(parser);

    size_t ptr_lvl = 0;
    while(match(parser, OP_STAR)) ptr_lvl++;

    expected(parser, IDENTIFIER, "Esperado nome do campo");

    Node *node = create_node_at(parser, NODE_VAR_DECL, peekprev(parser));
    node->ast.decl_variable.type = type_spec_from_node(type);
    node->ast.decl_variable.type.ptr_lvl = ptr_lvl;
    node->ast.decl_variable.constant = false;
    node->ast.decl_variable.init = NULL;
    node->ast.decl_variable.stattic = false;
    node->ast.decl_variable.name = peekprev(parser).value;

    expected(parser, SEMICOLON, "Esperado ';' apos o campo");

    return node;
}

static TypeSpec parse_array_suffix(Parser *parser, TypeSpec base) {
    Node* dims[MAX_ARRAY_DIMENTIONS] = { 0 };
    size_t count = 0;

    while(match(parser, OPEN_BRACKET)) {
        if(count >= MAX_ARRAY_DIMENTIONS) {
            diag_error(parser->context, peekprev(parser).filename, peekprev(parser).line, peekprev(parser).col,
                "numero maximo de dimensoes de array excedido");
            break;
        }

        if(peek(parser).type == CLOSE_BRACKET) {
            dims[count++] = NULL;
        } else {
            dims[count++] = parse_expr(parser);
        }

        expected(parser, CLOSE_BRACKET, "Esperava ']' apos o tamanho do array");
    }

    if(count > 0) {
        base.is_array = true;
        base.array_dim_count = count;
        base.array_dims = (Node**)arena_alloc(parser->arena, sizeof(Node*) * count);
        memcpy(base.array_dims, dims, sizeof(Node*) * count);
    }

    return base;
}

static Node* parse_aggregate_decl(Parser *parser, TokenType keyword, NodeKind kind, const char* key_name) {
    Token start = peek(parser);
    expected(parser, keyword, key_name);

    View tag = { 0 };
    bool has_tag = false;

    if(peek(parser).type == IDENTIFIER) {
        advance(parser);
        tag = peekprev(parser).value;
        has_tag = true;
    }

    Node* node = create_node_at(parser, kind, start);
    node->ast.aggregate.name = tag;
    node->ast.aggregate.members = NULL;
    node->ast.aggregate.tailing_decl = NULL;

    if(peek(parser).type == OPEN_BRACE) {
        node->ast.aggregate.members = list_create(parser->arena);
        expected(parser, OPEN_BRACE, "Esperado '{'");

        while(peek(parser).type != CLOSE_BRACE && peek(parser).type != EOFF) {
            Node* field = parse_field_decl(parser);
            list_push(parser->arena, node->ast.aggregate.members, field);
        }

        expected(parser, CLOSE_BRACE, "Esperado '}'");

        if(peek(parser).type == IDENTIFIER) {
            advance(parser);
            Node* decl = create_node_at(parser, NODE_VAR_DECL, peekprev(parser));
            decl->ast.decl_variable.type.base = IDENTIFIER;
            decl->ast.decl_variable.type.name = has_tag ? tag : (View){ 0 };
            decl->ast.decl_variable.type.nested = has_tag ? NULL : node;
            decl->ast.decl_variable.type.ptr_lvl = 0;
            decl->ast.decl_variable.name = peekprev(parser).value;
            decl->ast.decl_variable.init = NULL;

            expected(parser, SEMICOLON, "Esperado ';' apos a declaracao");

            node->ast.aggregate.tailing_decl = decl;
            return node;
        }

        expected(parser, SEMICOLON, "Esperado ';' apos a definicao");

        return node;
    }

    if(!has_tag) {
        diag_error(parser->context, peek(parser).filename, peek(parser).line, peek(parser).col,
            "Esperado '{' ou nome apos '%s'", key_name);
        return NULL;
    }

    expected(parser, IDENTIFIER, "Esperado nome");

    Node* decl = create_node_at(parser, NODE_VAR_DECL, peekprev(parser));
    decl->ast.decl_variable.type.base = IDENTIFIER;
    decl->ast.decl_variable.type.name = tag;
    decl->ast.decl_variable.type.nested = NULL;
    decl->ast.decl_variable.type.ptr_lvl = 0;
    decl->ast.decl_variable.name = peekprev(parser).value;
    decl->ast.decl_variable.init = NULL;

    expected(parser, SEMICOLON, "Esperado ';' apos o nome");

    return decl;
}

static Node* parse_call_stmt(Parser *parser) {
    Node* node = create_node(parser, NODE_CALL_STMT);
    expected(parser, KCALL, "Esperado call");

    if(peek(parser).type != SEMICOLON) node->ast.call_stmt.value = parse_expr(parser);
    else node->ast.call_stmt.value = NULL;

    expected(parser, SEMICOLON, "Esperado ';' apos a expressao");
    return node;
}

static Node* parse_newtype_decl(Parser *parser) {
    Token start = peek(parser);
    expected(parser, KNEWTYPE, "Esperado newtype");

    Node* type = parse_type_specifier(parser);

    expected(parser, IDENTIFIER, "Esperado nome do novo tipo");

    Node* node = create_node_at(parser, NODE_NEWTYPE, start);
    node->ast.newtype.name = peekprev(parser).value;
    node->ast.newtype.underlying = type_spec_from_node(type);

    expected(parser, SEMICOLON, "Esperado ';' apos a declaracao");
    return node;
}

static Node* parse_block_stmt(Parser *parser) {
    Node* node = create_node(parser, NODE_BLOCK);
    expected(parser, OPEN_BRACE, "Esperado '{' para abrir bloco");
    node->ast.program.statements = list_create(parser->arena);

    do {
        if(peek(parser).type == EOFF) break;
        if(peek(parser).type == CLOSE_BRACE) break;
        Node* stmt = parse_statement(parser);
        list_push(parser->arena, node->ast.program.statements, stmt);
    } while(peek(parser).type != CLOSE_BRACE);

    expected(parser, CLOSE_BRACE, "Esperado '}' para fechar bloco");
    return node;
}

static Node* parse_func_decl(Parser *parser, bool isstatic, bool isextern, Node *type, size_t ptr_lvl) {
    Node* node = create_node_at(parser, NODE_FUNC_DECL, peekprev(parser));
    node->ast.decl_function.call_type = type_spec_from_node(type);
    node->ast.decl_function.name = peekprev(parser).value;
    node->ast.decl_function.call_type.ptr_lvl = ptr_lvl;
    node->ast.decl_function.stattic = isstatic;
    node->ast.decl_function.variadic = false;
    node->ast.decl_function.params = list_create(parser->arena);
    node->ast.decl_function.exttern = isextern;

    if(isextern && isstatic) {
        diag_error(parser->context, peek(parser).filename, peek(parser).line, peek(parser).col,
            "Uma função extern não pode ser static ao mesmo tempo");
    }

    expected(parser, OPEN_PAREN, "Esperado '(' apos o nome da funcao");

    if(peek(parser).type != CLOSE_PAREN) {
        do {
            bool constant = match(parser, KCONST);
            Token token = peek(parser);

            if(token.type == ELLIPSES) {
                advance(parser);
                node->ast.decl_function.variadic = true;
                break;
            }

            Node* param_type = parse_type_specifier(parser);

            size_t ptr_lvl_param = 0;
            while(match(parser, OP_STAR)) ptr_lvl_param++;

            TypeSpec param_spec = type_spec_from_node(param_type);
            param_spec.ptr_lvl = ptr_lvl_param;

            if(param_spec.base == KVOID && ptr_lvl_param == 0) break;

            expected(parser, IDENTIFIER, "Esperado nome do parametro");

            Node* param = create_node_at(parser, NODE_VAR_DECL, peekprev(parser));
            param->ast.decl_variable.type = param_spec;
            param->ast.decl_variable.constant = constant;
            param->ast.decl_variable.init = NULL;
            param->ast.decl_variable.stattic = false;
            param->ast.decl_variable.name = peekprev(parser).value;

            list_push(parser->arena, node->ast.decl_function.params, param);
        } while(match(parser, COMMA));
    }

    expected(parser, CLOSE_PAREN, "Esperado ')' apos os parametros");

    if(peek(parser).type == SEMICOLON) {
        expected(parser, SEMICOLON, "Esperado ';' ou '{' apos a funcao");
        node->ast.decl_function.body = NULL;
    }
    else if(peek(parser).type == OPEN_BRACE) node->ast.decl_function.body = parse_statement(parser);
    else diag_error(parser->context, peek(parser).filename, peek(parser).line, peek(parser).col,
            "Esperado ';' ou '{' apos a funcao");

    if(node->ast.decl_function.exttern && node->ast.decl_function.body != NULL) {
        diag_error(parser->context, peek(parser).filename, peek(parser).line, peek(parser).col,
            "Uma função extern não pode conter corpo de função");
    }

    return node;
}

static Node* parse_var_decl(Parser *parser, bool isstatic, bool isconst, Node *type, size_t ptr_lvl) {
    Node* node = create_node_at(parser, NODE_VAR_DECL, peekprev(parser));
    node->ast.decl_variable.type = type_spec_from_node(type);
    node->ast.decl_variable.type.ptr_lvl = ptr_lvl;
    node->ast.decl_variable.constant = isconst;
    node->ast.decl_variable.stattic = isstatic;
    node->ast.decl_variable.name = peekprev(parser).value;
    node->ast.decl_variable.type = parse_array_suffix(parser, node->ast.decl_variable.type);

    if(match(parser, OPEN_BRACKET)) {
        node->ast.decl_variable.type.is_array = true;
        node->ast.decl_variable.type.ptr_lvl = 1;
    }

    if(match(parser, OP_ASSIGN)) node->ast.decl_variable.init = parse_expr(parser);
    else node->ast.decl_variable.init = NULL;

    expected(parser, SEMICOLON, "Esperado ';' apos a expressao");
    return node;
}

static Node* parse_decl(Parser *parser) {
    bool is_extern = match(parser, KEXTERN);
    bool is_static = match(parser, KSTATIC);
    bool is_const = match(parser, KCONST);

    Node *type = parse_type_specifier(parser);

    size_t ptr_lvl = 0;
    while(match(parser, OP_STAR)) ptr_lvl++;

    expected(parser, IDENTIFIER, "Esperado nome da funcao ou variavel");

    if(peek(parser).type == OPEN_PAREN)
        return parse_func_decl(parser, is_static, is_extern, type, ptr_lvl);
    return parse_var_decl(parser, is_static, is_const, type, ptr_lvl);
}

static Node* parse_if_stmt(Parser *parser) {
    Node* node = create_node(parser, NODE_IF_STMT);
    expected(parser, KIF, "Esperado 'if'");

    expected(parser, OPEN_PAREN, "Esperado '(' apos 'if'");
    node->ast.if_stmt.condition = parse_expr(parser);
    expected(parser, CLOSE_PAREN, "Esperado ')' apos a expressao");

    node->ast.if_stmt.then = parse_statement(parser);

    if(match(parser, KELSE)) node->ast.if_stmt.otherwise = parse_statement(parser);
    else node->ast.if_stmt.otherwise = NULL;

    return node;
}

static Node* parse_while_loop(Parser *parser) {
    Node* node = create_node(parser, NODE_WHILE_LOOP);
    expected(parser, KWHILE, "Esperado 'while'");

    expected(parser, OPEN_PAREN, "Esperado '(' apos o while");
    node->ast.while_loop.condition = parse_expr(parser);
    expected(parser, CLOSE_PAREN, "Esperado ')' apos a expressao");

    node->ast.while_loop.body = parse_statement(parser);
    
    return node;
}

static Node* parse_for_loop(Parser *parser) {
    Node* node = create_node(parser, NODE_FOR_LOOP);
    expected(parser, KFOR, "Esperado for");

    expected(parser, OPEN_PAREN, "Esperado '(' apos o for");

    if(peek(parser).type == SEMICOLON) {
        node->ast.for_loop.init = NULL;
        advance(parser);
    } else if(istype(peek(parser).type) || peek(parser).type == KCONST) {
        node->ast.for_loop.init = parse_decl(parser);
    } else {
        node->ast.for_loop.init = parse_expr(parser);
        expected(parser, SEMICOLON, "Esperado ';' apos a inicializacao");
    }

    if(peek(parser).type != SEMICOLON) node->ast.for_loop.condition = parse_expr(parser);
    else node->ast.for_loop.condition = NULL;

    expected(parser, SEMICOLON, "Esperado ';' apos a condicao");

    if(peek(parser).type != CLOSE_PAREN) node->ast.for_loop.increment = parse_expr(parser);
    else node->ast.for_loop.increment = NULL;

    expected(parser, CLOSE_PAREN, "Esperado ')' apos o incremento");

    node->ast.for_loop.body = parse_statement(parser);
    return node;
}

static Node* parse_break_stmt(Parser *parser) {
    Node* node = create_node(parser, NODE_BREAK_STMT);
    expected(parser, KBREAK, "Esperado break");
    expected(parser, SEMICOLON, "Esperado ';' apos o break");
    return node;
}

static Node* parse_continue_stmt(Parser *parser) {
    Node* node = create_node(parser, NODE_CONTINUE_STMT);
    expected(parser, KCONTINUE, "Esperado continue");
    expected(parser, SEMICOLON, "Esperado ';' apos o continue");
    return node;
}

static Node* parse_expr_stmt(Parser *parser) {
    Node* node = parse_expr(parser);
    expected(parser, SEMICOLON, "Esperado ';' apos a expressao");
    return node;
}

static Node* parse_enum_member(Parser *parser) {
    Node* node = create_node(parser, NODE_ENUM_MEMBER);

    expected(parser, IDENTIFIER, "Esperado nome do membro do enum");

    node->ast.enum_member.name = peekprev(parser).value;

    if(match(parser, OP_ASSIGN)) node->ast.enum_member.value = parse_expr(parser);
    else node->ast.enum_member.value = NULL;

    return node;
}

static Node* parse_enum_decl(Parser *parser) {
    Token start = peek(parser);

    expected(parser, KENUM, "Esperado enum");
    expected(parser, IDENTIFIER, "Esperado nome do enum");

    View name = peekprev(parser).value;

    Node* node = create_node_at(parser, NODE_ENUM, start);
    node->ast.enum_decl.name = name;
    node->ast.enum_decl.members = list_create(parser->arena);

    expected(parser, OPEN_BRACE, "Esperado '{' apos o nome");

    if(peek(parser).type != CLOSE_BRACE) {
        do {
            if(peek(parser).type == CLOSE_BRACE) break;
            Node* member = parse_enum_member(parser);
            list_push(parser->arena, node->ast.enum_decl.members, member);
        } while(match(parser, COMMA));
    }

    expected(parser, CLOSE_BRACE, "Esperado '}' apos o ultimo membro");
    expected(parser, SEMICOLON, "Esperado ';' apos o enum");

    return node;
}

static Node* parse_statement(Parser *parser) {
    TokenType current = peek(parser).type;

    if(current == KCALL) return parse_call_stmt(parser);
    if(current == OPEN_BRACE) return parse_block_stmt(parser);
    if(current == KIF) return parse_if_stmt(parser);
    if(istype(current) || current == KSTATIC || current == KCONST || current == KEXTERN) return parse_decl(parser);
    if(current == KWHILE) return parse_while_loop(parser);
    if(current == KFOR) return parse_for_loop(parser);
    if(current == KDATA) return parse_aggregate_decl(parser, KDATA, NODE_DATA, "data");
    if(current == KCOPERATE) return parse_aggregate_decl(parser, KCOPERATE, NODE_COPERATE, "coperate");
    if(current == KNEWTYPE) return parse_newtype_decl(parser);
    if(current == KCONTINUE) return parse_continue_stmt(parser);
    if(current == KBREAK) return parse_break_stmt(parser);
    if(current == KCALL) return parse_call_stmt(parser);
    if(current == KENUM) return parse_enum_decl(parser);
    if(current == IDENTIFIER || current == OP_STAR || current == OP_MINUS ||
        current == OP_BANG || current == OP_AND || current == OP_MINUS_MINUS || current == OP_PLUS_PLUS ||
        current == OPEN_PAREN) {
        return parse_expr_stmt(parser);
    }

    diag_error(parser->context, peek(parser).filename, peek(parser).line, peek(parser).col,
        "Comando ou declaracao inesperado");
    advance(parser);
    return NULL;
}

Node* parse_program(Parser *parser) {
    Node* program = create_node(parser, NODE_PROGRAM);
    program->ast.program.statements = list_create(parser->arena);

    while(peek(parser).type != EOFF) {
        Node* stmt = parse_statement(parser);
        if(stmt) list_push(parser->arena, program->ast.program.statements, stmt);
    }

    return program;
}
