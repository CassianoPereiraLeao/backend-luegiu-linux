#pragma once

#include "common.h"
#include "string_view_helper.h"

typedef struct {
    const char* start;
    const char* src;
    const char* cursor;
    const char* filename;

    size_t line;
    size_t col;
} Lexer;

typedef struct {
    TokenType type;
    size_t line;
    size_t col;
    const char* filename;
    View value;
} Token;

Lexer create_lexer(const char* src, const char* filename);
Token next_token(Lexer *lexer);
