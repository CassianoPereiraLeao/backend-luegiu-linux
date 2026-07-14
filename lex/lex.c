#include "lex.h"

Lexer create_lexer(const char* src, const char* filename, DiagContext *ctx) {
    Lexer lexer;
    lexer.col = 1;
    lexer.line = 1;
    lexer.cursor = src;
    lexer.start = src;
    lexer.source = src;
    lexer.context = ctx;
    lexer.filename = filename;
    return lexer;
}

static bool isend(Lexer *lexer) {
    return *lexer->cursor == '\0';
}

static char peek(Lexer *lexer) {
    if(isend(lexer)) return '\0';
    return *lexer->cursor;
}

static char advance(Lexer *lexer) {
    lexer->col++;
    return *lexer->cursor++;
}

bool isnumeric(char c) {
    return (c >= '0' && c <= '9');
}

bool ishexa(char c) {
    return (isnumeric(c) || (c >= 'A' && c <= 'F'));
}

static bool isalfa(char c) {
    return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
}

static bool isalfanumeric(char c) {
    return (isnumeric(c) || isalfa(c));
}

static bool isidentifier(char c) {
    return (isalfanumeric(c) || c == '_');
}

static bool strlcompare(View string, const char* match, size_t len) {
    if(string.len != len) return false;
    size_t i = 0;
    while(i < string.len) {
        if(string.start[i] != match[i]) return false;
        i++;
    }

    return (i == string.len && match[i] == '\0');
}

bool strcompare(View string, const char* match) {
    strlcompare(string, match, strlen(match));
}

static Token create_token(Lexer *lexer, TokenType type, size_t line, size_t col) {
    Token token;
    token.col = col;
    token.line = line;
    token.filename = lexer->filename;
    token.value.start = lexer->start;
    token.value.len = (size_t)(lexer->cursor - lexer->start);
    token.type = type;
    return token;
}

static TokenType check_keyword(View view) {
    switch (view.len)
    {
    case 2:
        if(strcompare(view, "if")) return KIF;
        if(strcompare(view, "do")) return KDO;
        if(strcompare(view, "up")) return KUP;
        break;
    case 3:
        if(strcompare(view, "for")) return KFOR;
        if(strcompare(view, "big")) return KBIG;
        break;
    case 4:
        if(strcompare(view, "else")) return KELSE;
        if(strcompare(view, "load")) return KLOAD;
        if(strcompare(view, "int8")) return KINT8;
        if(strcompare(view, "data")) return KDATA;
        if(strcompare(view, "hexa")) return KHEXA;
        if(strcompare(view, "link")) return KLINK;
        if(strcompare(view, "enum")) return KENUM;
        if(strcompare(view, "call")) return KCALL;
        if(strcompare(view, "void")) return KVOID;
        if(strcompare(view, "jump")) return KJUMP;
        break;
    case 5:
        if(strcompare(view, "small")) return KSMALL;
        if(strcompare(view, "const")) return KCONST;
        if(strcompare(view, "while")) return KWHILE;
        if(strcompare(view, "float")) return KFLOAT;
        if(strcompare(view, "uint8")) return KUINT8;
        if(strcompare(view, "int16")) return KINT16;
        if(strcompare(view, "int32")) return KINT32;
        if(strcompare(view, "int64")) return KINT64;
        break;
    case 6:
        if(strcompare(view, "static")) return KSTATIC;
        if(strcompare(view, "uint16")) return KUINT16;
        if(strcompare(view, "uint32")) return KUINT32;
        if(strcompare(view, "uint64")) return KUINT64;
        if(strcompare(view, "double")) return KDOUBLE;
        break;
    case 7:
        if(strcompare(view, "newtype")) return KNEWTYPE;
        break;
    case 8:
        if(strcompare(view, "continue")) return KCONTINUE;
        if(strcompare(view, "coperate")) return KCOPERATE;
        break;
    case 9:
        if(strcompare(view, "utf16char")) return KUTFCHAR;
        break;
    default: break;
    }

    return IDENTIFIER;
}

static Token identifier(Lexer *lexer, size_t line, size_t col) {
    advance(lexer);
    while(isidentifier(peek(lexer))) advance(lexer);

    View view = {
        lexer->start,
        (size_t)(lexer->cursor - lexer->start)
    };

    return create_token(lexer, check_keyword(view), line, col);
}

static Token hexalit(Lexer *lexer, size_t line, size_t col) {
    while(ishexa(peek(lexer))) advance(lexer);

    return create_token(lexer, HEXA_LIT, line, col);
}

static Token numericlit(Lexer *lexer, size_t line, size_t col) {
    if(peek(lexer) == '0') {
        advance(lexer);
        if(peek(lexer) == 'x') {
            advance(lexer);
            return hexalit(lexer, line, col);
        }
    }

    while(isnumeric(peek(lexer))) advance(lexer);
    if(peek(lexer) == '.') {
        advance(lexer);
        while(isnumeric(peek(lexer))) advance(lexer);
        if(peek(lexer) == 'f') {
            advance(lexer);
            return create_token(lexer, FLOAT_LIT, line, col);
        }
        return create_token(lexer, DOUBLE_LIT, line, col);
    }

    return create_token(lexer, INT_LIT, line, col);
}

static void skip(Lexer *lexer) {
    while(peek(lexer) == ' ' || peek(lexer) == '\n' || peek(lexer) == '\r' || peek(lexer) == '\t') {
        if(peek(lexer) == '\n') {
            lexer->line++;
            lexer->col = 0;
        }

        if(peek(lexer) == '\t') lexer->col += 3;

        advance(lexer);
    }
}

Token next_token(Lexer *lexer) {
    skip(lexer);
    size_t line = lexer->line;
    size_t col = lexer->col;
    lexer->start = lexer->cursor;
    char current = peek(lexer);

    if(current == '\0') return create_token(lexer, EOFF, line, col);

    if(isalfa(current) || current == '_') return identifier(lexer, line, col);

    if(isnumeric(current)) return numericlit(lexer, line, col);

    switch (current)
    {
    case ';': advance(lexer); return create_token(lexer, SEMICOLON, line, col);
    case ':': advance(lexer); return create_token(lexer, COLON, line, col);
    case '(': advance(lexer); return create_token(lexer, OPEN_PAREN, line, col);
    case ')': advance(lexer); return create_token(lexer, CLOSE_PAREN, line, col);
    case '[': advance(lexer); return create_token(lexer, OPEN_BRACKET, line, col);
    case ']': advance(lexer); return create_token(lexer, CLOSE_BRACKET, line, col);
    case '{': advance(lexer); return create_token(lexer, OPEN_BRACE, line, col);
    case '}': advance(lexer); return create_token(lexer, CLOSE_BRACE, line, col);
    case '?': advance(lexer); return create_token(lexer, OP_TERN, line, col);
    case ',': advance(lexer); return create_token(lexer, COMMA, line, col);
    case '~': advance(lexer); return create_token(lexer, OP_DESC, line, col);
    case '.': {
        advance(lexer);

        if(peek(lexer) == '.') {
            advance(lexer);
            if(peek(lexer) == '.') {
                advance(lexer);
                return create_token(lexer, ELLIPSES, line, col);
            }
            return create_token(lexer, UNDEFINED, line, col);
        }

        return create_token(lexer, DOT, line, col);
    }
    case '+': {
        advance(lexer);

        if(peek(lexer) == '=') {
            advance(lexer);
            return create_token(lexer, OP_PLUSEQ, line, col);
        }

        if(peek(lexer) == '+') {
            advance(lexer);
            return create_token(lexer, OP_PLUS_PLUS, line, col);
        }

        return create_token(lexer, OP_PLUS, line, col);
    }
    case '-': {
        advance(lexer);

        if(peek(lexer) == '=') {
            advance(lexer);
            return create_token(lexer, OP_MINUSEQ, line, col);
        }

        if(peek(lexer) == '-') {
            advance(lexer);
            return create_token(lexer, OP_MINUS_MINUS, line, col);
        }

        return create_token(lexer, OP_MINUS, line, col);
    }
    case '>': {
        advance(lexer);

        if(peek(lexer) == '>') {
            advance(lexer);
            if(peek(lexer) == '=') {
                advance(lexer);
                return create_token(lexer, OP_RSHIFTEQ, line, col);
            }
            return create_token(lexer, OP_RSHIFT, line, col);
        }

        if(peek(lexer) == '=') {
            advance(lexer);
            return create_token(lexer, OP_GE, line, col);
        }

        return create_token(lexer, OP_GT, line, col);
    }
    case '<': {
        advance(lexer);

        if(peek(lexer) == '<') {
            advance(lexer);
            if(peek(lexer) == '=') {
                advance(lexer);
                return create_token(lexer, OP_LSHIFTEQ, line, col);
            }
            return create_token(lexer, OP_LSHIFT, line, col);
        }

        if(peek(lexer) == '=') {
            advance(lexer);
            return create_token(lexer, OP_LE, line, col);
        }

        return create_token(lexer, OP_LT, line, col);
    }
    case '/': {
        advance(lexer);

        if(peek(lexer) == '=') {
            advance(lexer);
            return create_token(lexer, OP_SLASHEQ, line, col);
        }

        return create_token(lexer, OP_SLASH, line, col);
    }
    case '*': {
        advance(lexer);

        if(peek(lexer) == '=') {
            advance(lexer);
            return create_token(lexer, OP_STAREQ, line, col);
        }

        return create_token(lexer, OP_STAR, line, col);
    }
    case '%': {
        advance(lexer);

        if(peek(lexer) == '=') {
            advance(lexer);
            return create_token(lexer, OP_MODEQ, line, col);
        }

        return create_token(lexer, OP_MOD, line, col);
    }
    case '^': {
        advance(lexer);

        if(peek(lexer) == '=') {
            advance(lexer);
            return create_token(lexer, OP_XOREQ, line, col);
        }

        return create_token(lexer, OP_XOR, line, col);
    }
    case '&': {
        advance(lexer);

        if(peek(lexer) == '=') {
            advance(lexer);
            return create_token(lexer, OP_ANDEQ, line, col);
        }

        if(peek(lexer) == '&') {
            advance(lexer);
            return create_token(lexer, OP_LOGAND, line, col);
        }

        return create_token(lexer, OP_AND, line, col);
    }
    case '|': {
        advance(lexer);

        if(peek(lexer) == '|') {
            advance(lexer);
            return create_token(lexer, OP_LOGOR, line, col);
        }

        if(peek(lexer) == '=') {
            advance(lexer);
            return create_token(lexer, OP_OREQ, line, col);
        }

        return create_token(lexer, OP_OR, line, col);
    }
    case '!': {
        advance(lexer);

        if(peek(lexer) == '=') {
            advance(lexer);
            return create_token(lexer, OP_BANGEQ, line, col);
        }

        return create_token(lexer, OP_BANG, line, col);
    }
    case '"': {
        advance(lexer);

        if(peek(lexer) == '"') {
            advance(lexer);
            return create_token(lexer, UNDEFINED, line, col);
        }

        while(peek(lexer) != '"') {
            if(peek(lexer) == '\\') advance(lexer);
            advance(lexer);
        }

        advance(lexer);
        return create_token(lexer, STRING_LIT, line, col);
    }
    case '\'': {
        advance(lexer);
        
        if(peek(lexer) == '\\') advance(lexer);

        advance(lexer);

        if(peek(lexer) != '\'') return create_token(lexer, UNDEFINED, line, col);

        return create_token(lexer, CHAR_LIT, line, col);
    }
    default:
        advance(lexer);
        // if(lexer->context)
        //     diag_warning(lexer->context, );
        return create_token(lexer, UNDEFINED, line, col);
    }
}
