#pragma once

#include "common.h"

typedef enum {
    EOFF, UNDEFINED,
    
    IDENTIFIER, STRING_LIT, CHAR_LIT, HEXA_LIT,
    INT_LIT, FLOAT_LIT, DOUBLE_LIT, UNSIGNED_LIT,
    BIG_LIT,

    OPEN_PAREN, CLOSE_PAREN, OPEN_BRACKET,
    CLOSE_BRACKET, OPEN_BRACE, CLOSE_BRACE,

    COLON, SEMICOLON, ELLIPSES, COMMA, DOT,

    OP_LSHIFT, OP_RSHIFT, OP_PLUS, OP_PLUS_PLUS, OP_MINUS,
    OP_MINUS_MINUS, OP_MOD, OP_SLASH, OP_STAR, OP_BANG, OP_XOR,
    OP_AND, OP_OR, OP_GT, OP_LT, OP_DESC, OP_ARROW, OP_ASSIGN,

    OP_EQUALS, OP_LSHIFTEQ, OP_RSHIFTEQ, OP_PLUSEQ, OP_MINUSEQ,
    OP_SLASHEQ, OP_BANGEQ, OP_MODEQ, OP_STAREQ, OP_LOGOR, OP_LOGAND,
    OP_XOREQ, OP_OREQ, OP_ANDEQ, OP_GE, OP_LE, OP_TERN,

    KDATA, KENUM, KCOPERATE, KNEWTYPE, KWHILE, KFOR,
    KDO, KIF, KELSE, KCONST, KSTATIC, KATOMIC, KLOAD,
    KCALL, KBREAK, KCONTINUE, KJUMP, KEXTERN, KBYTES,

    KVOID, KSMALL, KLINK, KHEXA, KCHAR, KUTFCHAR,
    KFLOAT, KDOUBLE, KBIG, KUINT8, KUINT16, KUINT32,
    KUINT64, KINT8, KINT16, KINT32, KINT64
} TokenType;

typedef enum {
    NODE_UNDEFINED,
    NODE_VAR_DECL,
    NODE_FUNC_DECL,
    NODE_VAR_CALL,
    NODE_FUNC_CALL,
    NODE_LITERAL,
    NODE_BYTES,
    NODE_IF_STMT,
    NODE_BLOCK_STMT,
    NODE_CALL_STMT,
    NODE_BREAK_STMT,
    NODE_CONTINUE_STMT,
    NODE_FOR_LOOP,
    NODE_WHILE_LOOP,
    NODE_DO_WHILE_LOOP,
    NODE_DATA,
    NODE_ENUM,
    NODE_COPERATE,
    NODE_TERNARY,
    NODE_NEWTYPE,
    NODE_ENUM_MEMBER,
    NODE_ENUM_CALL,
    NODE_FIELD_CALL,
    NODE_CAST,
    NODE_PROGRAM,
    NODE_BINARY_OP,
    NODE_UNARY_OP,
    NODE_POSTFIX_OP,
    NODE_ARRAY,
    NODE_LOAD_LABEL,
    NODE_JUMP_LABEL,
    NODE_INIT_LIST,
} NodeKind;

typedef enum {
    INT,
    BIG,
    UNSIGNED,
    DOUBLE,
    HEXA,
    FLOAT,
    CHAR,
    STRING
} LiteralKind;

int isidentifier(char c);
int isnumeric(char c);
int isalphanumeric(char c);
int isletter(char c);
int ishexa(char c);

int view_equals_view(View first, View second);
int view_equals(View view, const char* str, size_t len);

const char* tk_to_str(TokenType token);
const char* node_to_str(NodeKind node);
const char* literal_to_str(LiteralKind literal);
