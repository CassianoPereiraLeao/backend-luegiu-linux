#pragma once

#include "common.h"
#include "lexer.h"
#include "arena.h"
#include "diagnostic.h"
#include "types.h"
#include "string_view_helper.h"

typedef struct NodeList NodeList;

typedef struct {
    TokenType base;
    View alias;

    struct Node* nested;

    uint8_t ptr_lvl;

    NodeList* array_dimentions;
    uint8_t is_array;
} TypeSpec;

typedef struct Node {
    NodeKind kind;
    const char* filename;
    size_t line;
    size_t col;
    struct Node* next;

    Typecheck resolved_type;
    size_t func_id;

    union {
        struct {
            View name;
        } call_variable;

        struct {
            TypeSpec call_type;
            View name;

            uint8_t _const;
            uint8_t _atomic;
            uint8_t _static;
            uint8_t _extern;

            struct Node* init;
        } decl_variable;

        struct {
            View name;
            NodeList* members;
        } enum_decl;

        struct {
            View name;
            struct Node* value;
        } enum_member;

        struct {
            View alias;
            View member;
            long long iresolved_type;
        } enum_access;

        struct {
            TypeSpec call_type;
            View name;

            NodeList* params;

            uint8_t _variadic;
            uint8_t _static;
            uint8_t _extern;
            uint8_t _const;
            uint8_t _atomic;

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
            View name;
            uint8_t arrow;
            size_t offset;
            Typecheck base_type;
        } field_access;

        struct {
            View alias;
            TypeSpec underlying;
        } newtype;

        struct {
            struct Node* operand;
            TokenType op;
        } unary;

        struct {
            TokenType op;
            struct Node* left;
            struct Node* right;
        } binary;

        struct {
            struct Node* condition;
            struct Node* body;
        } while_loop;

        struct {
            struct Node* init;
            struct Node* condition;
            struct Node* increment;
            struct Node* body;
        } for_loop;

        struct {
            struct Node* condition;
            struct Node* then;
            struct Node* otherwise;
        } if_stmt;
        

        struct {
            TypeSpec spec;
            struct Node* operand;
        } cast;

        struct {
            LiteralKind kind;
            long long integer64;
            unsigned long long unsigned64;
            long double double64;
            View character;
            View string;
        } literal;

        struct {
            View name;
        } jmp_label;

        struct {
            TypeSpec type;
            size_t size;
        } bytes;

        struct {
            NodeList* elements;
            bool is_zero;
        } init_list;

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
    Arena* arena;
    Token next;
    Token current;
    Token prev;
    DiagContext* ctx;
} Parser;

Parser create_parser(Lexer *lexer, DiagContext *ctx, Arena *arena);
Node* parse_program(Parser *parser);
