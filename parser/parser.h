#pragma once

#include "../common.h"
#include "../arena/arena.h"
#include "../lex/lex.h"
#include "types.h"

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
    NODE_BREAK_STMT,
    NODE_CONTINUE_STMT,
    NODE_CALL_STMT,
    NODE_BINARY_OP,
    NODE_UNARY_OP,
    NODE_POSTFIX_OP,
    NODE_ARRAY,
    NODE_LITERAL,
    NODE_DATA,
    NODE_COPERATE,
    NODE_NEWTYPE,
    NODE_ENUM,
    NODE_ENUM_MEMBER,
    NODE_ENUM_ACCESS,
    NODE_FIELD_ACCESS,
    NODE_TYPEREF,
    NODE_CAST,
    NODE_BLOCK,
    NODE_PROGRAM
} NodeKind;

typedef enum {
    INT,
    HEXA,
    DOUBLE,
    FLOAT,
    CHAR,
    STRING
} LiteralType;


typedef struct {
    TokenType base;
    View name;
    struct Node* nested;
    size_t ptr_lvl;
    bool is_array;
} TypeSpec;

typedef struct Node {
    NodeKind kind;
    struct Node* next;

    size_t line;
    size_t col;

    TypecheckType resolved_type;
    int func_id;

    const char* filename;
    union {
        struct {
            View name;
        } access_variable;

        struct {
            View name;
            NodeList* members;
        } enum_decl;

        struct {
            View name;
            struct Node* value;
        } enum_member;

        struct {
            View enum_name;
            View member_name;
            long long resolved_type;
        } enum_access;

        struct {
            TypeSpec type;
            View name;
            bool constant;
            bool stattic;
            struct Node* init;
        } decl_variable;

        struct {
            TokenType op;
            struct Node* operand;
        } unary_operator;

        struct {
            struct Node* condition;
            struct Node* body;
        } while_loop;

        struct {
            TokenType target;
            size_t ptr_lvl;
            struct Node* operand;
        } cast_expr;

        struct {
            struct Node* init;
            struct Node* condition;
            struct Node* increment;
            struct Node* body;
        } for_loop;

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
            TypeSpec call_type;
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
            struct Node* tailing_decl;
        } aggregate;

        struct {
            struct Node* base;
            View field_name;
            bool arrow;
            size_t field_offset;
            TypecheckType base_type;
        } field_access;

        struct {
            View name;
            TypeSpec underlying;
        } newtype;

        struct {
            TokenType primitive;
            View name;
        } type_ref;

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
