#pragma once

#include "../common.h"
#include "../arena/arena.h"
#include "../lex/lex.h"

typedef struct NodeList NodeList;

typedef enum {
    NODE_VAR_DECL,
    NODE_VAR_ACCESS,
    NODE_FUNC_DECL,
    NODE_FUNC_CALL,
    NODE_IF_STMT,
    NODE_FOR_LOOP,
    NODE_WHILE_LOOP,
    NODE_DO_WHILE_LOOP,
    NODE_BREAK_SMTM,
    NODE_CONTINUE_STMT,
    NODE_CALL_STMT,
    NODE_BINARY_OP,
    NODE_UNARY_OP,
    NODE_POSTFIX_OP,
    NODE_ARRAY,
    NODE_LITERAL,
    NODE_DATA,
    NODE_COPERATE,
    NODE_BLOCK,
    NODE_PROGRAM
} NodeKind;

typedef enum {
    PRIMITIVE,
    DATA,
    COPERATE,
    ENUM
} NewType;

typedef enum {
    INT,
    HEXA,
    DOUBLE,
    FLOAT,
    CHAR,
    STRING
} LiteralType;

typedef struct Node {
    NodeKind kind;
    struct Node* next;

    size_t line;
    size_t col;
    const char* filename;
    union {
        struct {
            View name;
        } access_variable;

        struct {
            View name;
            bool constant;
            bool stattic;
            TokenType data_type;
            size_t ptr_lvl;
            struct Node* init;
        } decl_variable;

        struct {
            TokenType op;
            struct Node* operand;
        } unary_operator;

        struct {
            TokenType op;
            struct Node* left;
            struct Node* right;
        } binary_operator;

        struct {
            LiteralType type;
            long long integer64;
            double double64;
            View character;
            View string;
        } literals;

        struct {
            struct Node* condition;
            struct Node* then;
            struct Node* otherwise;
        } if_stmt;

        struct {
            TokenType call_type;
            size_t ptr_lvl;
            View name;
            NodeList* params;
            bool variadic;
            bool stattic;
            struct Node* body;
        } decl_function;

        struct {
            View name;
            NodeList* args;
        } call_function;

        struct {
            struct Node* value;
        } call_stmt;

        struct {
            View name;
            NodeList* members;
        } data;

        struct {
            View name;
            NodeList* members;
        } coperate;

        struct {
            View name;
            struct Node* data;
        } newtype;

        struct {
            NodeList* statements;
        } program;
    } ast;
} Node;

struct NodeList {
    Node** items;
    size_t count;
    size_t capacity;
};

typedef struct {
    Lexer* lexer;
    Token current;
    Token previous;
    Arena* arena;
    DiagContext* context;
} Parser;

Parser create_parser(Lexer *lexer, Arena *arena, DiagContext *context);
Node* parse_program(Parser *parser);
