#pragma once

#include "../common.h"
#include "../lex/lex.h"

typedef struct AggregateDef AggregateDef;

typedef struct {
    TokenType base;
    View custom_name;
    AggregateDef* inline_def;
    size_t ptr_lvl;
    bool is_error;

    bool is_array;
    bool is_vla;
    size_t array_size;
} TypecheckType;
