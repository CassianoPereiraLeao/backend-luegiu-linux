#include "../../include/parser.h"
#include "../../include/string_view_helper.h"

static Node* parse_expr(Parser *parser);
static Node* parse_statement(Parser *parser);
static Node* parse_aggregate_decl_type_only(Parser *parser);
static Node* parse_field_decl(Parser *parser);
static Node* parse_enum_decl_type_only(Parser *parser);

typedef struct {
    size_t capacity;
    size_t count;
    View* types;
} Know_Newtype;

static Know_Newtype NewTypeTable = { 0 };

Parser create_parser(Lexer *lexer, DiagContext *ctx, Arena *arena) {
    Parser parser = { 0 };
    parser.arena = arena;
    parser.current = next_token(lexer);
    parser.next = next_token(lexer);
    parser.lexer = lexer;
    parser.ctx = ctx;
    return parser;
}

static NodeList* list_create(Arena *arena) {
    NodeList* list = (NodeList*)arena_alloc(arena, sizeof(NodeList));
    list->capacity = 4;
    list->count = 0;
    list->items = (Node**)arena_alloc(arena, sizeof(Node*) * 4);
    return list;
}

static void list_push(Arena *arena, NodeList* list, Node* item) {
    if(list->count >= list->capacity) {
        size_t new_cap = list->capacity * 2;
        Node** new_items = (Node**)arena_alloc(arena, sizeof(Node*) * new_cap);

        memcpy(new_items, list->items, sizeof(Node*) * list->count);

        list->capacity = new_cap;
        list->items = new_items;
    }

    list->items[list->count++] = item;
}

static bool check_known_newtype(View name) {
    for(size_t i = 0; i < NewTypeTable.count; ++i)
        if(view_equals_view(NewTypeTable.types[i], name)) return true;

    return false;
}

static void add_known_newtype(Arena *arena, View name) {
    if(NewTypeTable.count >= NewTypeTable.capacity) {
        size_t new_cap = (NewTypeTable.capacity == 0) ? 4 : NewTypeTable.capacity * 2;
        View* new_items = (View*)arena_alloc(arena, sizeof(View) * new_cap);

        memcpy(new_items, NewTypeTable.types, sizeof(View) * NewTypeTable.count);

        NewTypeTable.capacity = new_cap;
        NewTypeTable.types = new_items;
    }

    NewTypeTable.types[NewTypeTable.count++] = name;
}

static Token peek(Parser *parser) {
    return parser->current;
}

static Token peekprev(Parser *parser) {
    return parser->prev;
}

static Token peeknext(Parser *parser) {
    return parser->next;
}

static bool isend(Parser *parser) {
    return peek(parser).type == EOFF;
}

static Token advance(Parser *parser) {
    parser->prev = parser->current;
    parser->current = parser->next;
    parser->next = (isend(parser)) ? parser->current : next_token(parser->lexer);
    return parser->prev;
}

static Token expected(Parser *parser, TokenType expected, const char* fallback) {
    if(peek(parser).type != expected) {
        diag_error(parser->ctx, peek(parser).filename, peek(parser).line, peek(parser).col,
            fallback);
        advance(parser);
        return parser->prev;
    }

    return advance(parser);
}

static bool match(Parser *parser, TokenType type) {
    if(peek(parser).type != type) return false;

    advance(parser);
    return true;
}

static bool istypebase(TokenType type) {
    return (type >= KVOID && type <= KINT64);
}

static bool start_expression(TokenType type) {
    switch (type)
    {
    case HEXA_LIT:
    case INT_LIT:
    case BIG_LIT:
    case UNSIGNED_LIT:
    case CHAR_LIT:
    case STRING_LIT:
    case FLOAT_LIT:
    case DOUBLE_LIT:
    case OPEN_PAREN:
    case OP_STAR:
    case OP_DESC:
    case OP_AND:
    case OP_BANG:
    case OP_MINUS_MINUS:
    case OP_PLUS_PLUS:
    case KBYTES:
    case IDENTIFIER:
        return true;
    default: break;
    }

    return false;
}

static Node* create_node_at(Parser *parser, NodeKind kind, Token origin) {
    Node* node = (Node*)arena_alloc(parser->arena, sizeof(Node));
    memset(node, 0, sizeof(Node));
    node->kind = kind;
    node->line = origin.line;
    node->col = origin.col;
    node->filename = origin.filename;
    return node;
}

static Node* create_node(Parser *parser, NodeKind kind) {
    return create_node_at(parser, kind, peek(parser));
}

static TypeSpec parse_type_specifier(Parser *parser) {
    TypeSpec spec = { 0 };
    Token token = peek(parser);

    if(token.type == KDATA || token.type == KCOPERATE)
        return (TypeSpec){ .base = token.type, .nested = parse_aggregate_decl_type_only(parser) };
    if(token.type == KENUM)
        return (TypeSpec){ .base = KENUM, .nested = parse_enum_decl_type_only(parser) };

    if(istypebase(token.type)) {
        spec.base = token.type;
        advance(parser);
    } else if(token.type == IDENTIFIER) {
        spec.base = IDENTIFIER;
        spec.alias = token.value;
        advance(parser);
    } else {
        diag_error(parser->ctx, token.filename, token.line, token.col, 
            "Nao foi possivel analiser a palavra-chave: %s", tk_to_str(token.type));
        advance(parser);
    }

    return spec;
}

static TypeSpec parse_cast_type(Parser *parser) {
    TypeSpec spec = { 0 };
    Token token = peek(parser);

    if(token.type == KDATA || token.type == KCOPERATE) {
        spec.base = token.type;
        advance(parser);
        expected(parser, IDENTIFIER, (token.type == KDATA) ? "Esperado nome do data no cast" : "Esperado nome do coperate no cast");
        spec.alias = peekprev(parser).value;
    } else if(istypebase(token.type)) {
        spec.base = token.type;
        advance(parser);
    } else if(token.type == KENUM) {
        spec.base = token.type;
        advance(parser);
        expected(parser, IDENTIFIER, "Esperado nome do enum no cast");
        spec.alias = peekprev(parser).value;
    } else if(token.type == IDENTIFIER && check_known_newtype(token.value)) {
        spec.base = IDENTIFIER;
        spec.alias = token.value;
        advance(parser);
    } else {
        diag_error(parser->ctx, token.filename, token.line, token.col,
            "Tipo errado para cast");
        advance(parser);
    }

    while(match(parser, OP_STAR)) spec.ptr_lvl++;

    return spec;
}

static bool peeknext_starts_cast(Parser *parser) {
    Token next = peeknext(parser);
    TokenType type = next.type;

    if(istypebase(type)) return true;
    if(type == KDATA || type == KCOPERATE || type == KENUM) return true;
    if(type == IDENTIFIER && check_known_newtype(next.value)) return true;

    return false;
}

static size_t convert_hexa(Parser *parser, View value) {
    size_t result = 0;

    for(size_t i = 2; i < value.len; ++i) {
        char c = value.start[i];
        
        int digit = 0;
        if(isnumeric(c)) digit = c - '0';
        else if(c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else {
            diag_error(parser->ctx, peek(parser).filename, peek(parser).line, peek(parser).col,
                "Impossivel converter o caractere para hexadecimal: %c", c);
            return result;
        }

        result = result * 16 + digit;
    }

    return result;
}

static size_t convert_unsigned_int(Parser *parser, View value) {
    size_t result = 0;

    for(size_t i = 0; i < value.len; ++i) {
        char c = value.start[i];

        if(c == 'u' || c == 'U') break;
        if(isnumeric(c)) result = result * 10 + (c - '0');
        else {
            diag_error(parser->ctx, peek(parser).filename, peek(parser).line, peek(parser).col,
                "Impossivel converter caractere para inteiro sem cast: %c", c);
            return result;
        }
    }

    return result;
}

static long long convert_int(Parser *parser, View value) {
    long long result = 0;

    for(size_t i = 0; i < value.len; ++i) {
        char c = value.start[i];

        if(c == 'b' || c == 'B') break;
        if(isnumeric(c)) result = result * 10 + (c - '0');
        else {
            diag_error(parser->ctx, peek(parser).filename, peek(parser).line, peek(parser).col,
                "Impossivel converter caractere para inteiro sem cast: %c", c);
        }
    }

    return result;
}

static View remove_quote(View str) {
    str.start++;
    str.len -= 2;
    return str;
}

static Node* parse_primary(Parser *parser) {
    if(match(parser, UNSIGNED_LIT) || match(parser, HEXA_LIT)) {
        Node* node = create_node_at(parser, NODE_LITERAL, peekprev(parser));
        TokenType kind = peekprev(parser).type;
        if(kind == HEXA_LIT) node->ast.literal.unsigned64 = convert_hexa(parser, peekprev(parser).value);
        else node->ast.literal.unsigned64 = convert_unsigned_int(parser, peekprev(parser).value);
        node->ast.literal.kind = (kind == HEXA_LIT) ? HEXA : UNSIGNED;
        return node;
    }

    if(match(parser, INT_LIT) || match(parser, BIG_LIT)) {
        Node* node = create_node_at(parser, NODE_LITERAL, peekprev(parser));
        TokenType kind = peekprev(parser).type;
        node->ast.literal.kind = (kind == BIG_LIT) ? BIG : INT;
        node->ast.literal.integer64 = convert_int(parser, peekprev(parser).value);
        return node;
    }

    if(match(parser, FLOAT_LIT) || match(parser, DOUBLE_LIT)) {
        Node* node = create_node_at(parser, NODE_LITERAL, peekprev(parser));
        TokenType kind = peekprev(parser).type;
        node->ast.literal.kind = (kind == DOUBLE_LIT) ? DOUBLE : FLOAT;
        return node;
    }

    if(match(parser, CHAR_LIT)) {
        Node* node = create_node_at(parser, NODE_LITERAL, peekprev(parser));
        node->ast.literal.character = remove_quote(peekprev(parser).value);
        node->ast.literal.kind = CHAR;
        return node;
    }

    if(match(parser, STRING_LIT)) {
        Node* node = create_node_at(parser, NODE_LITERAL, peekprev(parser));
        node->ast.literal.string = remove_quote(peekprev(parser).value);
        node->ast.literal.kind = STRING;
        return node;
    }

    if(match(parser, IDENTIFIER)) {
        Token name = peekprev(parser);

        if(peek(parser).type == COLON) {
            advance(parser);
            expected(parser, OP_LT, "Esperado '<' apos ':' para acesso do enum");
            expected(parser, IDENTIFIER, "Esperado nome do membro do enum");

            Node* node = create_node_at(parser, NODE_ENUM_CALL, name);
            node->ast.enum_access.alias = name.value;
            node->ast.enum_access.member = peekprev(parser).value;

            return node;
        }

        Node* node = create_node_at(parser, NODE_VAR_CALL, name);
        node->ast.call_variable.name = name.value;

        return node;
    }

    if(match(parser, KBYTES)) {
        Node* node = create_node_at(parser, NODE_BYTES, peekprev(parser));
        expected(parser, OPEN_PAREN, "Esperado '(' apos o bytes");
        node->ast.bytes.type = parse_type_specifier(parser);
        expected(parser, CLOSE_PAREN, "Esperado ')' apos a expressao");
        return node;
    }

    if(match(parser, OPEN_PAREN)) {
        Node* node = parse_expr(parser);
        expected(parser, CLOSE_PAREN, "Esperado ')' apos a expressao");
        return node;
    }

    return create_node(parser, NODE_UNDEFINED);
}

static Node* parse_postfix(Parser *parser) {
    Node* expr = parse_primary(parser);

    for(;;) {
        if(match(parser, OP_MINUS_MINUS) || match(parser, OP_PLUS_PLUS)) {
            TokenType op = peekprev(parser).type;
            Node* node = create_node_at(parser, NODE_POSTFIX_OP, peekprev(parser));
            node->ast.unary.op = op;
            node->ast.unary.operand = expr;
            expr = node;
            continue;
        }

        if(match(parser, OPEN_BRACKET)) {
            Node* node = create_node_at(parser, NODE_ARRAY, peekprev(parser));
            node->ast.binary.left = expr;
            node->ast.binary.right = parse_expr(parser);
            expected(parser, CLOSE_BRACKET, "Esperado ']' apos a expressao");
            expr = node;
            continue;
        }

        if(match(parser, DOT) || match(parser, OP_ARROW)) {
            bool arrow = peekprev(parser).type == OP_ARROW;
            Token op = peekprev(parser);

            expected(parser, IDENTIFIER, "Esperado nome do campo apos o operador");

            Node* node = create_node_at(parser, NODE_FIELD_CALL, op);
            node->ast.field_access.arrow = arrow;
            node->ast.field_access.base = expr;
            node->ast.field_access.name = peekprev(parser).value;
            node->ast.field_access.offset = 0;
            expr = node;
            continue;
        }

        if(match(parser, OPEN_PAREN)) {
            if(expr->kind != NODE_VAR_CALL) {
                fprintf(stderr, "Erro aqui no parentese");
                continue;
            }

            Node* node = create_node_at(parser, NODE_FUNC_CALL, peekprev(parser));
            node->ast.call_function.name = expr->ast.call_variable.name;
            node->ast.call_function.args = list_create(parser->arena);

            do {
                if(match(parser, KVOID)) break;
                if(isend(parser)) break;
                if(peek(parser).type == CLOSE_PAREN) break;
                Node* arg = parse_expr(parser);
                list_push(parser->arena, node->ast.call_function.args, arg);
            } while(match(parser, COMMA));

            expected(parser, CLOSE_PAREN, "Esperado ')' apos os argumentos da funcao");
            expr = node;
            continue;
        }

        break;
    }

    return expr;
}

static Node* parse_unary(Parser *parser) {
    if(peek(parser).type == OPEN_PAREN && peeknext_starts_cast(parser)) {
        Token start = peek(parser);
        advance(parser);

        Node* node = create_node_at(parser, NODE_CAST, start);
        node->ast.cast.spec = parse_cast_type(parser);

        expected(parser, CLOSE_PAREN, "Esperado ')' apos o tipo do cast");

        node->ast.cast.operand = parse_unary(parser);
        return node;
    }

    if(match(parser, OP_MINUS) || match(parser, OP_MINUS_MINUS) || match(parser, OP_PLUS_PLUS) ||
        match(parser, OP_AND) || match(parser, OP_BANG) || match(parser, OP_STAR) || match(parser, OP_DESC)) {
        TokenType op = peekprev(parser).type;
        Node* node = create_node_at(parser, NODE_UNARY_OP, peekprev(parser));
        node->ast.unary.op = op;
        node->ast.unary.operand = parse_unary(parser);
        return node;
    }

    return parse_postfix(parser);
}

static Node* parse_factor(Parser *parser) {
    Node* expr = parse_unary(parser);

    while(match(parser, OP_STAR) || match(parser, OP_SLASH) || match(parser, OP_MOD)) {
        TokenType op = peekprev(parser).type;
        Node* node = create_node_at(parser, NODE_BINARY_OP, peekprev(parser));
        node->ast.binary.left = expr;
        node->ast.binary.right = parse_unary(parser);
        node->ast.binary.op = op;
        expr = node;
    }

    return expr;
}

static Node* parse_term(Parser *parser) {
    Node* expr = parse_factor(parser);

    while(match(parser, OP_PLUS) || match(parser, OP_MINUS)) {
        TokenType op = peekprev(parser).type;
        Node* node = create_node_at(parser, NODE_BINARY_OP, peekprev(parser));
        node->ast.binary.left = expr;
        node->ast.binary.right = parse_factor(parser);
        node->ast.binary.op = op;
        expr = node;
    }

    return expr;
}

static Node* parse_shift(Parser *parser) {
    Node* expr = parse_term(parser);

    while(match(parser, OP_LSHIFT) || match(parser, OP_RSHIFT)) {
        TokenType op = peekprev(parser).type;
        Node* node = create_node_at(parser, NODE_BINARY_OP, peekprev(parser));
        node->ast.binary.op = op;
        node->ast.binary.left = expr;
        node->ast.binary.right = parse_term(parser);
        expr = node;
    }

    return expr;
}

static Node* parse_comparison(Parser *parser) {
    Node* expr = parse_shift(parser);

    while(match(parser, OP_LT) || match(parser, OP_LE) || match(parser, OP_GT) || match(parser, OP_GE)) {
        TokenType op = peekprev(parser).type;
        Node* node = create_node_at(parser, NODE_BINARY_OP, peekprev(parser));
        node->ast.binary.op = op;
        node->ast.binary.left = expr;
        node->ast.binary.right = parse_shift(parser);
        expr = node;
    }

    return expr;
}

static Node* parse_equality(Parser *parser) {
    Node* expr = parse_comparison(parser);

    while(match(parser, OP_EQUALS) || match(parser, OP_BANGEQ)) {
        TokenType op = peekprev(parser).type;
        Node* node = create_node_at(parser, NODE_BINARY_OP, peekprev(parser));
        node->ast.binary.left = expr;
        node->ast.binary.right = parse_comparison(parser);
        node->ast.binary.op = op;
        expr = node;
    }

    return expr;
}

static Node* parse_bitwise_and(Parser *parser) {
    Node* expr = parse_equality(parser);

    while(match(parser, OP_AND)) {
        TokenType op = peekprev(parser).type;
        Node* node = create_node_at(parser, NODE_BINARY_OP, peekprev(parser));
        node->ast.binary.left = expr;
        node->ast.binary.right = parse_equality(parser);
        node->ast.binary.op = op;
        expr = node;
    }

    return expr;
}

static Node* parse_bitwise_xor(Parser *parser) {
    Node* expr = parse_bitwise_and(parser);

    while(match(parser, OP_XOR)) {
        TokenType op = peekprev(parser).type;
        Node* node = create_node_at(parser, NODE_BINARY_OP, peekprev(parser));
        node->ast.binary.left = expr;
        node->ast.binary.right = parse_bitwise_and(parser);
        node->ast.binary.op = op;
        expr = node;
    }

    return expr;
}

static Node* parse_bitwise_or(Parser *parser) {
    Node* expr = parse_bitwise_xor(parser);

    while(match(parser, OP_OR)) {
        TokenType op = peekprev(parser).type;
        Node* node = create_node_at(parser, NODE_BINARY_OP, peekprev(parser));
        node->ast.binary.left = expr;
        node->ast.binary.right = parse_bitwise_xor(parser);
        node->ast.binary.op = op;
        expr = node;
    }

    return expr;
}

static Node* parse_logical_and(Parser *parser) {
    Node* expr = parse_bitwise_or(parser);

    while(match(parser, OP_LOGAND)) {
        Node* node = create_node_at(parser, NODE_BINARY_OP, peekprev(parser));
        TokenType op = peekprev(parser).type;
        node->ast.binary.op = op;
        node->ast.binary.left = expr;
        node->ast.binary.right = parse_bitwise_or(parser);
        expr = node;
    }

    return expr;
}

static Node* parse_logical_or(Parser *parser) {
    Node* expr = parse_logical_and(parser);

    while(match(parser, OP_LOGOR)) {
        Node* node = create_node_at(parser, NODE_BINARY_OP, peekprev(parser));
        TokenType op = peekprev(parser).type;
        node->ast.binary.op = op;
        node->ast.binary.left = expr;
        node->ast.binary.right = parse_logical_and(parser);
        expr = node;
    }

    return expr;
}

static Node* parse_ternary(Parser *parser) {
    Node* expr = parse_logical_or(parser);

    if(match(parser, OP_TERN)) {
        Node* node = create_node_at(parser, NODE_TERNARY, peekprev(parser));
        node->ast.if_stmt.condition = expr;
        node->ast.if_stmt.then = parse_expr(parser);

        expected(parser, COLON, "Esperado ':' apos o campo verdadeiro");

        node->ast.if_stmt.otherwise = parse_expr(parser);

        return node;
    }

    return expr;
}

static Node* parse_assignment(Parser *parser) {
    Node* expr = parse_ternary(parser);

    if(match(parser, OP_ASSIGN) || match(parser, OP_PLUSEQ) || match(parser, OP_MINUSEQ) || match(parser, OP_ANDEQ) ||
        match(parser, OP_XOREQ) || match(parser, OP_OREQ) || match(parser, OP_MODEQ) || match(parser, OP_SLASHEQ) ||
        match(parser, OP_STAREQ) || match(parser, OP_RSHIFTEQ) || match(parser, OP_LSHIFTEQ)) {
        TokenType op = peekprev(parser).type;
        Node* node = create_node_at(parser, NODE_BINARY_OP, peekprev(parser));
        node->ast.binary.left = expr;
        node->ast.binary.right = parse_expr(parser);
        node->ast.binary.op = op;
        expr = node;
    }

    return expr;
}

static Node* parse_expr(Parser *parser) {
    return parse_assignment(parser);
}

static Node* parse_expr_statement(Parser *parser) {
    Node* node = parse_expr(parser);
    expected(parser, SEMICOLON, "Esperado ';' apos a expressao");
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

static Node* parse_call_stmt(Parser *parser) {
    Node* node = create_node(parser, NODE_CALL_STMT);
    
    expected(parser, KCALL, "Esperado call");

    if(peek(parser).type == SEMICOLON) node->ast.call_stmt.value = NULL;
    else node->ast.call_stmt.value = parse_expr(parser);

    expected(parser, SEMICOLON, "Esperado ';' apos a expressao");
    return node;
}

static Node* parse_init_list(Parser *parser) {
    Node* node = create_node(parser, NODE_INIT_LIST);
    node->ast.init_list.elements = list_create(parser->arena);
    node->ast.init_list.is_zero = false;

    expected(parser, OPEN_BRACE, "Esperado '{' para inicializar a lista");

    if(peek(parser).type != CLOSE_BRACE) {
        do {
            if(isend(parser)) break;
            if(peek(parser).type == CLOSE_BRACE) break;

            Node* element = { 0 };
            if(peek(parser).type == OPEN_BRACE) element = parse_init_list(parser);
            else element = parse_expr(parser);

            list_push(parser->arena, node->ast.init_list.elements, element);
        } while(match(parser, COMMA));
    }

    expected(parser, CLOSE_PAREN, "Esperado '}' apos a lista");

    NodeList* elements = node->ast.init_list.elements;
    if(elements->count == 1 && elements->items[0]->kind == NODE_LITERAL && 
        elements->items[0]->ast.literal.kind == INT &&
        elements->items[0]->ast.literal.integer64 == 0)
        node->ast.init_list.is_zero = true;

    return node;
}

static Node* parse_var_decl(Parser *parser, bool isstatic, bool isconst, bool isextern, bool isatomic, Token init, TypeSpec type) {
    Node* node = create_node_at(parser, NODE_VAR_DECL, init);
    node->ast.decl_variable.call_type = type;
    node->ast.decl_variable._atomic = isatomic;
    node->ast.decl_variable._const = isconst;
    node->ast.decl_variable._extern = isextern;
    node->ast.decl_variable._static = isstatic;
    node->ast.decl_variable.name = peekprev(parser).value;

    if(match(parser, OPEN_BRACKET)) {
        node->ast.decl_variable.call_type.is_array = true;
        node->ast.decl_variable.call_type.array_dimentions = list_create(parser->arena);

        do {
            if(isend(parser)) break;
            Node* dim = parse_expr(parser);
            node->ast.decl_variable.call_type.ptr_lvl++;
            expected(parser, CLOSE_BRACKET, "Esperado ']' apos a expressao");
            list_push(parser->arena, node->ast.decl_variable.call_type.array_dimentions, dim);
        } while(match(parser, OPEN_BRACKET));
    }

    if(match(parser, OP_ASSIGN)) { 
        node->ast.decl_variable.init = (peek(parser).type == OPEN_BRACE)
            ? parse_init_list(parser)
            : parse_expr(parser);
    }
    else node->ast.decl_variable.init = NULL;

    expected(parser, SEMICOLON, "Esperado ';' apos a declaracao de variavel");

    return node;
}

static Node* parse_function_decl(Parser *parser, bool isconst, bool isextern, bool isatomic, bool isstatic, Token nodeinit, TypeSpec type) {
    Node* node = create_node_at(parser, NODE_FUNC_DECL, nodeinit);
    node->ast.decl_function._extern = isextern;
    node->ast.decl_function._const = isconst;
    node->ast.decl_function._static = isstatic;
    node->ast.decl_function._atomic = isatomic;
    node->ast.decl_function.name = peekprev(parser).value;
    node->ast.decl_function.call_type = type;
    node->ast.decl_function.params = list_create(parser->arena);

    if(isstatic && isextern) {
        fprintf(stdout, "Uma funcao ou varivel nao pode ser static e extern ao mesmo tempo");
    }

    expected(parser, OPEN_PAREN, "Esperado '(' para declarar a funcao");

    if(peek(parser).type != CLOSE_PAREN) {
        do {
            Token initial_token = peek(parser);
            bool _constant = match(parser, KCONST);
            bool _atomic = match(parser, KATOMIC);
            bool _static = match(parser, KSTATIC);
            bool _extern = match(parser, KEXTERN);

            if(_static || _extern) {
                fprintf(stdout, "A funcao nao pode ter parametros static ou extern"); NLSTDOUT;
                break;
            }

            Token tk_arg = peek(parser);

            if(tk_arg.type == ELLIPSES) {
                advance(parser);
                node->ast.decl_function._variadic = true;
                break;
            }

            TypeSpec param_type = parse_type_specifier(parser);

            uint8_t arg_ptr_lvl = 0;
            while(match(parser, OP_STAR)) arg_ptr_lvl++;

            if(arg_ptr_lvl == 0 && param_type.base == KVOID) break;

            param_type.ptr_lvl = arg_ptr_lvl;
            expected(parser, IDENTIFIER, "Esperado nome do parametro");

            Node* param = create_node_at(parser, NODE_VAR_DECL, initial_token);
            param->ast.decl_variable.call_type = param_type;
            param->ast.decl_variable.init = NULL;
            param->ast.decl_variable.name = peekprev(parser).value;
            param->ast.decl_variable._atomic = _atomic;
            param->ast.decl_variable._const = _constant;
            param->ast.decl_variable._static = false;
            param->ast.decl_variable._extern = false;

            list_push(parser->arena, node->ast.decl_function.params, param);
        } while(match(parser, COMMA));
    }

    expected(parser, CLOSE_PAREN, "Esperado ')' apos os parametros");

    if(peek(parser).type == SEMICOLON) {
        expected(parser, SEMICOLON, "Esperado ';' apos a funcao prototipa");
        node->ast.decl_function.body = NULL;
    } else if(peek(parser).type == OPEN_BRACE) {
        node->ast.decl_function.body = parse_statement(parser);
    } else {
        advance(parser);
        fprintf(stdout, "Esperado ')' ou ';' apos a expressao"); NLSTDOUT;
    }

    if(node->ast.decl_function._extern && node->ast.decl_function.body != NULL) {
        fprintf(stdout, "Uma funcao definida como extern nao pode conter corpo");
    }

    return node;
}

static Node* parse_aggregate_decl_type_only(Parser *parser) {
    Token start = peek(parser);
    NodeKind kind = (start.type == KDATA) ? NODE_DATA : NODE_COPERATE; 
    Node* node = create_node_at(parser, kind, start);

    expected(parser, start.type, (start.type == KDATA) ? "Esperado data" : "Esperado coperate");

    View tag = { 0 };
    if(peek(parser).type == IDENTIFIER) {
        advance(parser);
        tag = peekprev(parser).value;
    }

    node->ast.aggregate.name = tag;
    node->ast.aggregate.members = NULL;
    node->ast.aggregate.tailing_decl = NULL;

    if(peek(parser).type == OPEN_BRACE) {
        node->ast.aggregate.members = list_create(parser->arena);
        expected(parser, OPEN_BRACE, "Esperado '{' apos a declaracao");

        while(peek(parser).type != CLOSE_BRACE && !isend(parser)) {
            Node* field = parse_field_decl(parser);
            list_push(parser->arena, node->ast.aggregate.members, field);
        }

        expected(parser, CLOSE_BRACE, "Esperado '}' apos os campos");
    }

    return node;
}

static Node* parse_field_decl(Parser *parser) {
    bool _constant = match(parser, KCONST);
    bool _atomic = match(parser, KATOMIC);
    bool _static = match(parser, KSTATIC);
    bool _extern = match(parser, KEXTERN);

    if(_static || _extern) fprintf(stdout, "O campo nao pode ser definido como static ou extern");

    TypeSpec type = parse_type_specifier(parser);
    Token init = peekprev(parser);

    uint8_t ptr_lvl = 0;
    while(match(parser, OP_STAR)) ptr_lvl++;

    expected(parser, IDENTIFIER, "Esperado nome do campo");

    Node* node = create_node_at(parser, NODE_VAR_DECL, init);
    node->ast.decl_variable.call_type = type;
    node->ast.decl_variable.call_type.ptr_lvl = ptr_lvl;
    node->ast.decl_variable._const = _constant;
    node->ast.decl_variable._static = _static;
    node->ast.decl_variable._atomic = _atomic;
    node->ast.decl_variable._extern = _extern;
    node->ast.decl_variable.init = NULL;
    node->ast.decl_variable.name = peekprev(parser).value;

    expected(parser, SEMICOLON, "Esperado ';' apos a declaracao");
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

static Node* parse_enum_decl_type_only(Parser *parser) {
    Node* node = create_node(parser, NODE_ENUM);

    expected(parser, KENUM, "Esperado enum");
    
    View tag = { 0 };
    if(peek(parser).type == IDENTIFIER) {
        advance(parser);
        tag = peekprev(parser).value;
    }

    node->ast.enum_decl.name = tag;
    node->ast.enum_decl.members = NULL;

    if(peek(parser).type == OPEN_BRACE) {
        advance(parser);
        node->ast.enum_decl.members = list_create(parser->arena);

        if(peek(parser).type != CLOSE_BRACE) {
            do {
                if(isend(parser)) break;
                if(peek(parser).type == CLOSE_BRACE) break;
                Node* member = parse_enum_member(parser);
                list_push(parser->arena, node->ast.enum_decl.members, member);
            } while(match(parser, COMMA));
        }
        expected(parser, CLOSE_BRACE, "Esperado '}' apos os membros do enum");
    }

    return node;
}

static Node* parse_decl(Parser *parser) {
    bool _static = match(parser, KSTATIC);
    bool _extern = match(parser, KEXTERN);
    bool _const = match(parser, KCONST);
    bool _atomic = match(parser, KATOMIC);

    if(_static && _extern) 
        diag_error(parser->ctx, peekprev(parser).filename, peekprev(parser).line, peekprev(parser).col,
            "Nao e possivel uma variavel/funcao ser static e extern ao mesmo tempo");

    Token init = peek(parser);

    TypeSpec call_type = parse_type_specifier(parser);

    if((call_type.base == KDATA || call_type.base == KENUM || call_type.base == KCOPERATE) &&
        peek(parser).type == SEMICOLON) {
            advance(parser);
            return call_type.nested;
    }

    uint8_t ptr_lvl = 0;
    while(match(parser, OP_STAR)) ptr_lvl++;

    call_type.ptr_lvl = ptr_lvl;

    if(peeknext(parser).type == OPEN_PAREN) expected(parser, IDENTIFIER, "Esperado nome da funcao");
    else expected(parser, IDENTIFIER, "Esperado nome da variavel");

    if(peek(parser).type == OPEN_PAREN)
        return parse_function_decl(parser, _const, _extern, _atomic, _static, init, call_type);
    return parse_var_decl(parser, _static, _const, _extern, _atomic, init, call_type);
}

static Node* parse_block_stmt(Parser *parser) {
    Node* node = create_node(parser, NODE_BLOCK_STMT);

    expected(parser, OPEN_BRACE, "Esperado '{' para inicializar o bloco");

    node->ast.program.statements = list_create(parser->arena);

    do {
        if(isend(parser)) break;
        if(peek(parser).type == CLOSE_BRACE) break;

        Node* statement = parse_statement(parser);
        list_push(parser->arena, node->ast.program.statements, statement);
    } while(peek(parser).type != CLOSE_BRACE);

    expected(parser, CLOSE_BRACE, "Esperado '}' para finalizar o bloco");
    return node;
}

static Node* parse_while_loop(Parser *parser) {
    Node* node = create_node(parser, NODE_WHILE_LOOP);

    expected(parser, KWHILE, "Esperado while para comecar o loop");
    expected(parser, OPEN_PAREN, "Esperado '(' apos o while");

    node->ast.while_loop.condition = parse_expr(parser);

    expected(parser, CLOSE_PAREN, "Esperado ')' apos a condicao");

    node->ast.while_loop.body = parse_statement(parser);

    return node;
}

static Node* parse_do_while_loop(Parser *parser) {
    Node* node = create_node(parser, NODE_DO_WHILE_LOOP);

    expected(parser, KDO, "Esperado do para comecar o loop");

    node->ast.while_loop.body = parse_block_stmt(parser);

    expected(parser, KWHILE, "Esperado while apos o '}'");
    expected(parser, OPEN_PAREN, "Esperado '(' apos o while");

    node->ast.while_loop.condition = parse_expr(parser);

    expected(parser, CLOSE_PAREN, "Esperado ')' apos a expressao");
    expected(parser, SEMICOLON, "Esperado ';' apos o while");
    return node;
}

static Node* parse_for_loop(Parser *parser) {
    Node* node = create_node(parser, NODE_FOR_LOOP);

    expected(parser, KFOR, "Esperado for para comecar o for-loop");
    expected(parser, OPEN_PAREN, "Esperado '(' apos o for");

    if(peek(parser).type == SEMICOLON) { 
        node->ast.for_loop.init = NULL; 
        advance(parser); 
    }
    else if(peek(parser).type == KCONST || peek(parser).type == KATOMIC || istypebase(peek(parser).type))
        node->ast.for_loop.init = parse_decl(parser);
    else { 
        node->ast.for_loop.init = parse_expr(parser); 
        expected(parser, SEMICOLON, "Esperado ';' apos a inicializacao"); 
    }

    if(peek(parser).type == SEMICOLON) node->ast.for_loop.condition = NULL;
    else node->ast.for_loop.condition = parse_expr(parser);

    expected(parser, SEMICOLON, "Esperado ';' apos a condicao");

    if(peek(parser).type == CLOSE_PAREN) node->ast.for_loop.increment = NULL;
    else node->ast.for_loop.increment = parse_expr(parser);

    expected(parser, CLOSE_PAREN, "Esperado ')' apos o incremento");

    node->ast.for_loop.body = parse_statement(parser);

    return node;
}

static Node* parse_newtype_decl(Parser *parser) {
    Node* node = create_node(parser, NODE_NEWTYPE);

    expected(parser, KNEWTYPE, "Esperado newtype");

    TypeSpec type = parse_type_specifier(parser);

    expected(parser, IDENTIFIER, "Esperado nome do tipo");

    node->ast.newtype.alias = peekprev(parser).value;
    node->ast.newtype.underlying = type;
    add_known_newtype(parser->arena, peekprev(parser).value);

    expected(parser, SEMICOLON, "Esperado ';' apos a criacao do novo newtype");

    return node;
}

static Node* parse_if_stmt(Parser *parser) {
    Node* node = create_node(parser, NODE_IF_STMT);

    expected(parser, KIF, "Esperado if");
    expected(parser, OPEN_PAREN, "Esperado '(' apos o if");

    node->ast.if_stmt.condition = parse_expr(parser);

    expected(parser, CLOSE_PAREN, "Esperado ')' apos a expressao");

    node->ast.if_stmt.then = parse_statement(parser);

    if(match(parser, KELSE)) node->ast.if_stmt.otherwise = parse_statement(parser);
    else node->ast.if_stmt.otherwise = NULL;

    return node;
}

static Node* parse_jump_label(Parser *parser) {
    Node* node = create_node(parser, NODE_JUMP_LABEL);

    expected(parser, KJUMP, "Esperado jump");
    expected(parser, OP_MOD, "Esperado '%' antes do nome do label");
    expected(parser, IDENTIFIER, "Esperado nome do label");

    node->ast.jmp_label.name = peekprev(parser).value;

    expected(parser, SEMICOLON, "Esperado ';' apos o nome do label");
    
    return node;
}

static Node* parse_load_label(Parser *parser) {
    Node* node = create_node(parser, NODE_LOAD_LABEL);

    expected(parser, KLOAD, "Esperado load");
    expected(parser, OP_MOD, "Esperado '%' antes do nome do label");
    expected(parser, IDENTIFIER, "Esperado nome do label");

    node->ast.jmp_label.name = peekprev(parser).value;

    expected(parser, COLON, "Esperado ':' apos o nome do label");

    return node;
}

static Node* parse_statement(Parser *parser) {
    TokenType current = peek(parser).type;

    if(current == KCALL) return parse_call_stmt(parser);
    if(current == KBREAK) return parse_break_stmt(parser);
    if(current == KCONTINUE) return parse_continue_stmt(parser);
    if(istypebase(current) || current == KATOMIC || current == KSTATIC || current == KEXTERN ||
        current == KCONST) return parse_decl(parser);
    if(current == KDATA || current == KCOPERATE || current == KENUM) return parse_decl(parser);
    if(current == IDENTIFIER && check_known_newtype(peek(parser).value)) return parse_decl(parser);
    if(current == KNEWTYPE) return parse_newtype_decl(parser);
    if(current == OPEN_BRACE) return parse_block_stmt(parser);
    if(current == KWHILE) return parse_while_loop(parser);
    if(current == KFOR) return parse_for_loop(parser);
    if(current == KDO) return parse_do_while_loop(parser);
    if(current == KIF) return parse_if_stmt(parser);
    if(current == KJUMP) return parse_jump_label(parser);
    if(current == KLOAD) return parse_load_label(parser);
    if(start_expression(current)) return parse_expr_statement(parser);

    fprintf(stdout, "Pulando Token: %s:%zu:%zu File: %s", tk_to_str(current), peek(parser).line, peek(parser).col, peek(parser).filename); NLSTDOUT;
    advance(parser);
    return create_node(parser, NODE_UNDEFINED);
}

static void init_known_types(Arena *arena) {
    NewTypeTable.capacity = 4;
    NewTypeTable.count = 0;
    NewTypeTable.types = (View*)arena_alloc(arena, sizeof(View) * 4);
}

Node* parse_program(Parser *parser) {
    init_known_types(parser->arena);
    Node* program = create_node(parser, NODE_PROGRAM);
    program->ast.program.statements = list_create(parser->arena);

    while(!isend(parser)) {
        Node* statement = parse_statement(parser);
        if(statement != NULL)
            list_push(parser->arena, program->ast.program.statements, statement);
    }

    return program;
}
