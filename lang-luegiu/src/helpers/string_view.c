#include "../../include/string_view_helper.h"

int isnumeric(char c) {
    return (c >= '0' && c <= '9');
}

int isletter(char c) {
    return (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z');
}

int isidentifier(char c) {
    return (isletter(c) || c == '_');
}

int isalphanumeric(char c) {
    return (isidentifier(c) || isnumeric(c));
}

int ishexa(char c) {
    return (isnumeric(c) || (c >= 'A' && c <= 'F'));
}

int view_equals(View view, const char* str, size_t len) {
    if(view.len != len) return 0;

    for(size_t i = 0; i < len; ++i)
        if(view.start[i] != str[i]) return 0;

    return str[len] == '\0';
}

int view_equals_view(View first, View second) {
    if(first.len != second.len) return 0;
    if(first.len == 0) return 0;
    return memcmp(first.start, second.start, first.len) == 0;
}

const char* tk_to_str(TokenType token) {
    switch (token)
    {
    case OP_GT: return "GREATER"; case OP_GE: return "GREATER OR EQUAL";
    case OP_LT: return "LESS"; case OP_LE: return "LESS OR EQUAL";
    case OP_AND: return "BIT AND"; case OP_ANDEQ: return "AND EQUALS"; case OP_LOGAND: return "LOG AND";
    case OP_OR: return "BIT OR"; case OP_OREQ: return "OR EQUALS"; case OP_LOGOR: return "LOG OR";
    case OP_XOR: return "XOR"; case OP_XOREQ: return "XOR EQUALS";
    case OP_ASSIGN: return "ASSIGN"; case OP_EQUALS: return "EQUALS";
    case OP_MINUS: return "MINUS"; case OP_MINUS_MINUS: return "MINUS MINUS"; case OP_MINUSEQ: return "MINUS EQUALS"; case OP_ARROW: return "ARROW";
    case OP_PLUS: return "PLUS"; case OP_PLUS_PLUS: return "PLUS PLUS"; case OP_PLUSEQ: return "PLUS EQUALS";
    case OP_STAR: return "STAR"; case OP_STAREQ: return "STAR EQUALS";
    case OP_SLASH: return "SLASH"; case OP_SLASHEQ: return "SLASH EQUALS";
    case OP_RSHIFT: return "RIGHT SHIFT"; case OP_RSHIFTEQ: return "RIGHT SHIFT EQUALS";
    case OP_LSHIFT: return "LEFT SHIFT"; case OP_LSHIFTEQ: return "LEFT SHIFT EQUALS";
    case KLINK: return "LINK"; case KATOMIC: return "ATOMIC"; case KBIG: return "BIG";
    case KCALL: return "CALL"; case KCHAR: return "KCHAR"; case KCONST: return "CONST";
    case KCONTINUE: return "CONTINUE"; case KCOPERATE: return "COPERATE"; case KDATA: return "DATA";
    case KDO: return "DO"; case KDOUBLE: return "DOUBLE"; case KFLOAT: return "FLOAT";
    case KBREAK: return "BREAK"; case KELSE: return "ELSE"; case KIF: return "IF";
    case KENUM: return "ENUM"; case KEXTERN: return "EXTERN"; case KFOR: return "FOR";
    case KHEXA: return "HEXA"; case KINT16: return "INT16"; case KINT8: return "INT8";
    case KINT32: return "INT32"; case KINT64: return "INT64"; case KUINT8: return "UINT8";
    case KUINT16: return "UINT16"; case KUINT32: return "UINT32"; case KUINT64: return "UINT64";
    case KLOAD: return "LOAD"; case KJUMP: return "JUMP"; case KNEWTYPE: return "NEWTYPE";
    case KSMALL: return "SMALL"; case KSTATIC: return "STATIC"; case KUTFCHAR: return "UTFCHAR";
    case KVOID: return "VOID"; case KWHILE: return "WHILE"; case IDENTIFIER: return "IDENTIFIER";
    case STRING_LIT: return "STRING_LIT"; case INT_LIT: return "INT_LIT"; case CHAR_LIT: return "CHAR_LIT";
    case HEXA_LIT: return "HEXA_LIT"; case DOUBLE_LIT: return "DOUBLE_LIT"; case FLOAT_LIT: return "FLOAT_LIT";
    case UNSIGNED_LIT: return "UNSIGNED_LIT"; case SEMICOLON: return "SEMICOLON"; case COLON: return "COLON";
    case DOT: return "DOT"; case OP_MOD: return "MOD"; case OP_MODEQ: return "MOD EQUALS";
    case OP_BANG: return "BANG"; case OP_BANGEQ: return "BANG EQUALS"; case COMMA: return "COMMA";
    case ELLIPSES: return "ELLIPSES"; case OP_TERN: return "TERNARIO"; case EOFF: return "end";
    case BIG_LIT: return "BIG_LIT"; case OPEN_PAREN: return "OPEN PAREN"; case CLOSE_PAREN: return "CLOSE PAREN";
    case OPEN_BRACKET: return "OPEN BRACKET"; case CLOSE_BRACKET: return "CLOSE BRACKET"; case OPEN_BRACE: return "OPEN BRACE";
    case CLOSE_BRACE: return "CLOSE BRACE"; case UNDEFINED: return "UNDEFINED"; case OP_DESC: return "DESC";
    case KBYTES: return "BYTES";
    }

    return "?";
}

const char* node_to_str(NodeKind node) {
    switch (node)
    {
    case NODE_ARRAY: return "NODE_ARRAY"; case NODE_BINARY_OP: return "NODE_BINARY_OP";
    case NODE_BLOCK_STMT: return "NODE_BLOCK_STMT"; case NODE_BREAK_STMT: return "NODE_BREAK_STMT";
    case NODE_CALL_STMT: return "NODE_CALL_STMT"; case NODE_CAST: return "NODE_CAST";
    case NODE_CONTINUE_STMT: return "NODE_CONTINUE_STMT"; case NODE_COPERATE: return "NODE_COPERATE";
    case NODE_DATA: return "NODE_DATA"; case NODE_DO_WHILE_LOOP: return "NODE_DO_WHILE_LOOP";
    case NODE_ENUM: return "NODE_ENUM"; case NODE_ENUM_CALL: return "NODE_ENUM_CALL";
    case NODE_ENUM_MEMBER: return "NODE_ENUM_MEMBER"; case NODE_FIELD_CALL: return "NODE_FIELD_CALL";
    case NODE_FOR_LOOP: return "NODE_FOR_LOOP"; case NODE_FUNC_CALL: return "NODE_FUNC_CALL";
    case NODE_FUNC_DECL: return "NODE_FUNC_DECL"; case NODE_IF_STMT: return "NODE_IF_STMT";
    case NODE_LITERAL: return "NODE_LITERAL"; case NODE_NEWTYPE: return "NODE_NEWTYPE";
    case NODE_POSTFIX_OP: return "NODE_POSTIFIX_OP"; case NODE_PROGRAM: return "NODE_PROGRAM";
    case NODE_UNARY_OP: return "NODE_UNARY_OP"; case NODE_VAR_CALL: return "NODE_VAR_CALL";
    case NODE_VAR_DECL: return "NODE_VAR_DECL"; case NODE_WHILE_LOOP: return "NODE_WHILE_LOOP";
    case NODE_TERNARY: return "NODE_TERNARY"; case NODE_UNDEFINED: return "NODE_UNDEFINED";
    case NODE_JUMP_LABEL: return "NODE_JMP_LABEL"; case NODE_LOAD_LABEL: return "NODE_LOAD_LABEL";
    case NODE_BYTES: return "NODE_BYTES";
    }

    return "?";
}

const char* literal_to_str(LiteralKind literal) {
    switch (literal)
    {
    case INT: return "INT";
    case UNSIGNED: return "UNSGINED";
    case STRING: return "STRING";
    case CHAR: return "CHAR";
    case FLOAT: return "FLOAT";
    case BIG: return "BIG";
    case HEXA: return "HEXA";
    case DOUBLE: return "DOUBLE";
    }

    return "?";
}
