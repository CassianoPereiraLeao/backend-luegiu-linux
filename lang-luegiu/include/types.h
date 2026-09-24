#pragma once

#include "common.h"
#include "lexer.h"
#include "string_view_helper.h"

typedef struct AggregateDef AggregateDef;

typedef struct {
    TokenType base;
    View alias;
    AggregateDef* inline_def;
    uint8_t ptr_lvl;
    uint8_t error;
    uint8_t _atomic;
    uint8_t _array;
    uint8_t _vla;
    size_t array_size;
} Typecheck;
