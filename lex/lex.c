#include "lex.h"

static Lexer create_lexer(const char* src, const char* filename, DiagContext *ctx) {
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
    
}
