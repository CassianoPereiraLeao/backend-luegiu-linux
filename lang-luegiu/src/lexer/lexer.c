#include "../../include/lexer.h"

static bool isend(char c) {
    return c == '\0';
}

static char peek(Lexer *lexer) {
    return *lexer->cursor;
}

static char advance(Lexer *lexer) {
    char c = *lexer->cursor++;
    lexer->col++;
    return c;
}

static void skip(Lexer *lexer) {
    for(;;) {
        if(peek(lexer) == ' ' || peek(lexer) == '\r') {
            advance(lexer); 
            continue;
        }

        if(peek(lexer) == '\n') {
            advance(lexer);
            lexer->col = 1;
            lexer->line++;
            continue;
        }

        if(peek(lexer) == '\t') {
            advance(lexer);
            lexer->col += 3;
            continue;
        }

        break;
    }
}

static Token make_token(Lexer *lexer, TokenType type, size_t line, size_t col) {
    Token token = { 0 };
    token.type = type;
    token.filename = lexer->filename;
    token.line = line;
    token.col = col;
    token.value.len = (size_t)(lexer->cursor - lexer->start);
    token.value.start = lexer->start;
    return token;
}

static TokenType check_keyword(View view) {
    switch (view.len)
    {
    case 2:
        if(view_equals(view, "if", 2)) return KIF;
        if(view_equals(view, "do", 2)) return KDO;
        break;

    case 3:
        if(view_equals(view, "for", 3)) return KFOR;
        if(view_equals(view, "big", 3)) return KBIG;
        break;

    case 4:
        if(view_equals(view, "int8", 4)) return KINT8;
        if(view_equals(view, "load", 4)) return KLOAD;
        if(view_equals(view, "else", 4)) return KELSE;
        if(view_equals(view, "void", 4)) return KVOID;
        if(view_equals(view, "link", 4)) return KLINK;
        if(view_equals(view, "jump", 4)) return KJUMP;
        if(view_equals(view, "hexa", 4)) return KHEXA;
        if(view_equals(view, "char", 4)) return KCHAR;
        if(view_equals(view, "call", 4)) return KCALL;
        if(view_equals(view, "enum", 4)) return KENUM;
        if(view_equals(view, "data", 4)) return KDATA;
        break;

    case 5:
        if(view_equals(view, "break", 5)) return KBREAK;
        if(view_equals(view, "while", 5)) return KWHILE;
        if(view_equals(view, "const", 5)) return KCONST;
        if(view_equals(view, "int16", 5)) return KINT16;
        if(view_equals(view, "int32", 5)) return KINT32;
        if(view_equals(view, "int64", 5)) return KINT64;
        if(view_equals(view, "uint8", 5)) return KUINT8;
        if(view_equals(view, "small", 5)) return KSMALL;
        if(view_equals(view, "float", 5)) return KFLOAT;
        if(view_equals(view, "bytes", 5)) return KBYTES;
        break;

    case 6:
        if(view_equals(view, "double", 6)) return KDOUBLE;
        if(view_equals(view, "atomic", 6)) return KATOMIC;
        if(view_equals(view, "static", 6)) return KSTATIC;
        if(view_equals(view, "uint16", 6)) return KUINT16;
        if(view_equals(view, "uint32", 6)) return KUINT32;
        if(view_equals(view, "uint64", 6)) return KUINT64;
        if(view_equals(view, "extern", 6)) return KEXTERN;
        break;

    case 7:
        if(view_equals(view, "newtype", 7)) return KNEWTYPE;
        break;

    case 8:
        if(view_equals(view, "coperate", 8)) return KCOPERATE;
        if(view_equals(view, "continue", 8)) return KCONTINUE;
        break;

    case 9: 
        if(view_equals(view, "utf16char", 9)) return KUTFCHAR;
        break;
    }

    return IDENTIFIER;
}

static Token make_identifier(Lexer *lexer, size_t line, size_t col) {
    while(!isend(peek(lexer)) && (isalphanumeric(peek(lexer)) || peek(lexer) == '_')) advance(lexer);

    View view = {
        lexer->start,
        (size_t)(lexer->cursor - lexer->start)
    };

    return make_token(lexer, check_keyword(view), line, col);
}

static Token make_hexa(Lexer *lexer, size_t line, size_t col) {
    while(ishexa(peek(lexer))) advance(lexer);

    return make_token(lexer, HEXA_LIT, line, col);
}

static Token make_number(Lexer *lexer, size_t line, size_t col) {
    while(isnumeric(peek(lexer)) && !isend(peek(lexer))) advance(lexer);

    if(peek(lexer) == 'x') {
        advance(lexer);
        return make_hexa(lexer, line, col);
    }

    if(peek(lexer) == '.') {
        advance(lexer);
        while(isnumeric(peek(lexer)) && !isend(peek(lexer))) advance(lexer);
        if(isletter(peek(lexer))) {
            if(peek(lexer) == 'f') { advance(lexer); return make_token(lexer, FLOAT_LIT, line, col); }
            return make_token(lexer, UNDEFINED, line, col);
        }
        
        return make_token(lexer, DOUBLE_LIT, line, col);
    }

    if(isletter(peek(lexer))) {
        if(peek(lexer) == 'U' || peek(lexer) == 'u') { advance(lexer); return make_token(lexer, UNSIGNED_LIT, line, col); }
        if(peek(lexer) == 'B' || peek(lexer) == 'b') { advance(lexer); return make_token(lexer, BIG_LIT, line, col); }
        return make_token(lexer, UNDEFINED, line, col);
    }

    return make_token(lexer, INT_LIT, line, col);
}

Lexer create_lexer(const char* src, const char* filename) {
    Lexer lexer;
    lexer.src = src;
    lexer.start = src;
    lexer.cursor = src;
    lexer.line = 1;
    lexer.col = 1;
    lexer.filename = filename;
    return lexer;
}

Token next_token(Lexer *lexer) {
    skip(lexer);
    size_t line = lexer->line;
    size_t col = lexer->col;
    char current = peek(lexer);
    lexer->start = lexer->cursor;

    if(isend(current)) return make_token(lexer, EOFF, line, col);

    if(isidentifier(current)) { advance(lexer); return make_identifier(lexer, line, col); }
    if(isnumeric(current)) { advance(lexer); return make_number(lexer, line, col); }

    switch (current)
    {
    case '(': advance(lexer); return make_token(lexer, OPEN_PAREN, line, col);
    case ')': advance(lexer); return make_token(lexer, CLOSE_PAREN, line, col);
    case '[': advance(lexer); return make_token(lexer, OPEN_BRACKET, line, col);
    case ']': advance(lexer); return make_token(lexer, CLOSE_BRACKET, line, col);
    case '{': advance(lexer); return make_token(lexer, OPEN_BRACE, line, col);
    case '}': advance(lexer); return make_token(lexer, CLOSE_BRACE, line, col);
    case ':': advance(lexer); return make_token(lexer, COLON, line, col);
    case ';': advance(lexer); return make_token(lexer, SEMICOLON, line, col);
    case ',': advance(lexer); return make_token(lexer, COMMA, line, col);
    case '~': advance(lexer); return make_token(lexer, OP_DESC, line, col);
    case '?': advance(lexer); return make_token(lexer, OP_TERN, line, col);
    case '.': {
        advance(lexer);

        if(peek(lexer) == '.') {
            advance(lexer);
            if(peek(lexer) == '.') {
                advance(lexer);
                return make_token(lexer, ELLIPSES, line, col);
            }

            return make_token(lexer, UNDEFINED, line, col);
        }

        return make_token(lexer, DOT, line, col);
    }

    case '>': {
        advance(lexer);

        if(peek(lexer) == '>') {
            advance(lexer);
            if(peek(lexer) == '=') {
                advance(lexer);
                return make_token(lexer, OP_RSHIFTEQ, line, col);
            }
            return make_token(lexer, OP_RSHIFT, line, col);
        }

        if(peek(lexer) == '=') {
            advance(lexer);
            return make_token(lexer, OP_GE, line, col);
        }

        return make_token(lexer, OP_GT, line, col);
    }

    case '<': {
        advance(lexer);

        if(peek(lexer) == '<') {
            advance(lexer);
            if(peek(lexer) == '=') {
                advance(lexer);
                return make_token(lexer, OP_LSHIFTEQ, line, col);
            }
            return make_token(lexer, OP_LSHIFT, line, col);
        }

        if(peek(lexer) == '=') {
            advance(lexer);
            return make_token(lexer, OP_LE, line, col);
        }

        return make_token(lexer, OP_LT, line, col);
    }

    case '=': {
        advance(lexer);

        if(peek(lexer) == '=') {
            advance(lexer);
            return make_token(lexer, OP_EQUALS, line, col);
        }

        return make_token(lexer, OP_ASSIGN, line, col);
    }

    case '-': {
        advance(lexer);

        if(peek(lexer) == '-') {
            advance(lexer);
            return make_token(lexer, OP_MINUS_MINUS, line, col);
        }

        if(peek(lexer) == '=') {
            advance(lexer);
            return make_token(lexer, OP_MINUSEQ, line, col);
        }

        if(peek(lexer) == '>') {
            advance(lexer);
            return make_token(lexer, OP_ARROW, line, col);
        }

        return make_token(lexer, OP_MINUS, line, col);
    }

    case '+': {
        advance(lexer);
        
        if(peek(lexer) == '+') {
            advance(lexer);
            return make_token(lexer, OP_PLUS_PLUS, line, col);
        }

        if(peek(lexer) == '=') {
            advance(lexer);
            return make_token(lexer, OP_PLUSEQ, line, col);
        }

        return make_token(lexer, OP_PLUS, line, col);
    }

    case '/': {
        advance(lexer);

        if(peek(lexer) == '=') {
            advance(lexer);
            return make_token(lexer, OP_SLASHEQ, line, col);
        }

        return make_token(lexer, OP_SLASH, line, col);
    }

    case '%': {
        advance(lexer);

        if(peek(lexer) == '=') {
            advance(lexer);
            return make_token(lexer, OP_MODEQ, line, col);
        }

        return make_token(lexer, OP_MOD, line, col);
    }

    case '*': {
        advance(lexer);

        if(peek(lexer) == '=') {
            advance(lexer);
            return make_token(lexer, OP_STAREQ, line, col);
        }

        return make_token(lexer, OP_STAR, line, col);
    }

    case '"': {
        advance(lexer);

        while(peek(lexer) != '"' && !isend(peek(lexer))) {
            if(peek(lexer) == '\\') advance(lexer);
            advance(lexer);
        }

        advance(lexer);
        
        return make_token(lexer, STRING_LIT, line, col);
    }

    case '\'': {
        advance(lexer);

        if(peek(lexer) == '\'' || isend(peek(lexer))) {
            advance(lexer);
            return make_token(lexer, UNDEFINED, line, col);
        }

        if(peek(lexer) == '\\') advance(lexer);
        advance(lexer);

        advance(lexer); // esse advanced consome o '

        return make_token(lexer, CHAR_LIT, line, col);
    }

    case '^': {
        advance(lexer);

        if(peek(lexer) == '=') {
            advance(lexer);
            return make_token(lexer, OP_XOREQ, line, col);
        }

        return make_token(lexer, OP_XOR, line, col);
    }

    case '&': {
        advance(lexer);

        if(peek(lexer) == '&') {
            advance(lexer);
            return make_token(lexer, OP_LOGAND, line, col);
        }

        if(peek(lexer) == '=') {
            advance(lexer);
            return make_token(lexer, OP_ANDEQ, line, col);
        }

        return make_token(lexer, OP_AND, line, col);
    }

    case '|': {
        advance(lexer);

        if(peek(lexer) == '|') {
            advance(lexer);
            return make_token(lexer, OP_LOGOR, line, col);
        }

        if(peek(lexer) == '=') {
            advance(lexer);
            return make_token(lexer, OP_OREQ, line, col);
        }

        return make_token(lexer, OP_OR, line, col);
    }

    case '!': {
        advance(lexer);

        if(peek(lexer) == '=') {
            advance(lexer);
            return make_token(lexer, OP_BANGEQ, line, col);
        }

        return make_token(lexer, OP_BANG, line, col);
    }
    }

    advance(lexer);
    return make_token(lexer, UNDEFINED, line, col);
}
