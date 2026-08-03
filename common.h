#pragma once

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>

typedef struct DiagContext DiagContext;

typedef enum {
    EOFF,
    UNDEFINED,
    
    IDENTIFIER,
    STRING_LIT,
    CHAR_LIT,
    HEXA_LIT,
    INT_LIT,
    FLOAT_LIT,
    DOUBLE_LIT,

    OPEN_PAREN,
    CLOSE_PAREN,
    OPEN_BRACKET,
    CLOSE_BRACKET,
    OPEN_BRACE,
    CLOSE_BRACE,

    COLON,
    SEMICOLON,
    ELLIPSES,
    COMMA,
    DOT,

    OP_LSHIFT,
    OP_RSHIFT,
    OP_PLUS,
    OP_PLUS_PLUS,
    OP_MINUS,
    OP_MINUS_MINUS,
    OP_MOD,
    OP_SLASH,
    OP_STAR,
    OP_BANG,
    OP_XOR,
    OP_AND,
    OP_OR,
    OP_GT,
    OP_LT,
    OP_DESC,
    OP_ARROW,
    OP_ASSIGN,

    OP_EQUALS,
    OP_LSHIFTEQ,
    OP_RSHIFTEQ,
    OP_PLUSEQ,
    OP_MINUSEQ,
    OP_SLASHEQ,
    OP_BANGEQ,
    OP_MODEQ,
    OP_STAREQ,
    OP_LOGOR,
    OP_LOGAND,
    OP_XOREQ,
    OP_OREQ,
    OP_ANDEQ,
    OP_GE,
    OP_LE,
    OP_TERN,

    KDATA,
    KENUM,
    KCOPERATE,
    KNEWTYPE,
    KWHILE,
    KFOR,
    KDO,
    KIF,
    KELSE,
    KCONST,
    KSTATIC,
    KLOAD,
    KCALL,
    KBREAK,
    KCONTINUE,
    KUP,
    KJUMP,
    KEXTERN,

    KVOID,
    KSMALL,
    KLINK,
    KHEXA,
    KCHAR,
    KUTFCHAR,
    KWIDE,
    KFLOAT,
    KDOUBLE,
    KBIG,
    KUINT8,
    KUINT16,
    KUINT32,
    KUINT64,
    KINT8,
    KINT16,
    KINT32,
    KINT64
} TokenType;

typedef struct {
    const char* start;
    size_t len;
} View;

typedef struct {
    const char* start;
    const char* source;
    const char* cursor;
    const char* filename;

    size_t col;
    size_t line;

    DiagContext* context;
} Lexer;

typedef struct {
    TokenType type;
    View value;

    size_t col;
    size_t line;

    const char* filename;
} Token;

typedef struct ArenaChunk {
    struct ArenaChunk* next;
    size_t used;
    size_t capacity;
    uint8_t data[];
} ArenaChunk;

typedef struct {
    ArenaChunk* first;
    ArenaChunk* last;
    size_t size;
} Arena;
