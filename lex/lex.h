#pragma once

#include "../common.h"

bool isnumeric(char c);
bool ishexa(char c);
Lexer create_lexer(const char* src, const char* filename, DiagContext *ctx);
Token next_token(Lexer *lexer);