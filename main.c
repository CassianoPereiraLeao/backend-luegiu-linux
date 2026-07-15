#include "common.h"
#include "./lex/lex.h"
#include "./arena/arena.h"
#include "./diagnostics/diagnostics.h"

void out_lexer(const char* path) {
    FILE* f = fopen(path, "rb");
    if(!f) {
        fprintf(stderr, "Arquivo nao encontrado para a analise\n");
        exit(7);
    }

    fseek(f, 0, SEEK_END);
    long tell = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* src = malloc(tell + 1);

    size_t bytes = fread(src, sizeof(char), tell, f);
    if(tell < bytes) {
        fprintf(stderr, "Falha ao ler o arquivo alvo");
        exit(6);
    }

    fclose(f);

    src[bytes] = '\0';

    FILE* out = fopen("./debug/exit.luegiu.lex", "wb");

    Arena arena;
    arena_init(&arena, 1024 * 1024);

    DiagContext context;
    diag_init(&context, &arena);
    diag_set_max_errors(&context, 200);

    Lexer lexer = create_lexer(src, path, &context);

    if(has_error(&context)) {
        diag_report_all(&context);
        free(src);
        arena_free(&arena);
        return;
    }

    fprintf(out, "Analise Lexica: \n");
    Token type;
    while((type = next_token(&lexer)).type != EOFF) {
        fprintf(out, "Token: ");
        switch (type.type)
        {
        case KVOID:                     fprintf(out, "TOKEN_VOID          "); break;
        case IDENTIFIER:                fprintf(out, "IDENTIFIER          "); break;
        case OPEN_PAREN:                fprintf(out, "OPEN_PAREN          "); break;
        case CLOSE_PAREN:               fprintf(out, "CLOSE_PAREN         "); break;
        case OPEN_BRACKET:              fprintf(out, "OPEN_BRACKET        "); break;
        case CLOSE_BRACKET:             fprintf(out, "CLOSE_BRACKET       "); break;
        case OPEN_BRACE:                fprintf(out, "OPEN_BRACE          "); break;
        case CLOSE_BRACE:               fprintf(out, "CLOSE_BRACE         "); break;
        case INT_LIT:                   fprintf(out, "INT_LIT             "); break;
        case KINT8:                     fprintf(out, "KINT8               "); break;
        case KINT16:                    fprintf(out, "KINT16              "); break;
        case KINT32:                    fprintf(out, "KINT32              "); break;
        case KINT64:                    fprintf(out, "KINT64              "); break;
        case KUINT8:                    fprintf(out, "KUINT8              "); break;
        case KUINT16:                   fprintf(out, "KUINT16             "); break;
        case KUINT32:                   fprintf(out, "KUINT32             "); break;
        case KUINT64:                   fprintf(out, "KUINT64             "); break;
        case KBIG:                      fprintf(out, "KBIG                "); break;
        case KSMALL:                    fprintf(out, "KSMALL              "); break;
        case KCALL:                     fprintf(out, "KCALL               "); break;
        case UNDEFINED:                 fprintf(out, "UNDEFINED           "); break;
        case SEMICOLON:                 fprintf(out, "SEMICOLON           "); break;
        case COLON:                     fprintf(out, "COLON               "); break;
        case DOT:                       fprintf(out, "DOT                 "); break;
        case ELLIPSES:                  fprintf(out, "ELLIPSES            "); break;
        case KCHAR:                     fprintf(out, "KCHAR               "); break;
        case CHAR_LIT:                  fprintf(out, "CHAR_LIT            "); break;
        case KDOUBLE:                   fprintf(out, "KDOUBLE             "); break;
        case DOUBLE_LIT:                fprintf(out, "DOUBLE_LIT          "); break;
        case KHEXA:                     fprintf(out, "KHEXA               "); break;
        case HEXA_LIT:                  fprintf(out, "HEXA_LIT            "); break;
        case KFLOAT:                    fprintf(out, "KFLOAT              "); break;
        case FLOAT_LIT:                 fprintf(out, "FLOAT_LIT           "); break;
        case STRING_LIT:                fprintf(out, "STRING_LIT          "); break;
        case COMMA:                     fprintf(out, "COMMA               "); break;
        case OP_LSHIFT:                 fprintf(out, "OP_LSHIFT           "); break;
        case OP_RSHIFT:                 fprintf(out, "OP_RSHIFT           "); break;
        case OP_LSHIFTEQ:               fprintf(out, "OP_LSHIFTEQ         "); break;
        case OP_RSHIFTEQ:               fprintf(out, "OP_RSHIFTEQ         "); break;
        case KCOPERATE:                 fprintf(out, "KCOPERATE           "); break;
        case KDATA:                     fprintf(out, "KDATA               "); break;
        case KNEWTYPE:                  fprintf(out, "KNEWTYPE            "); break;
        case KLINK:                     fprintf(out, "KLINK               "); break;
        case OP_ASSIGN:                 fprintf(out, "OP_ASSIGN           "); break;
        case OP_EQUALS:                 fprintf(out, "OP_EQUALS           "); break;
        case OP_STAR:                   fprintf(out, "OP_STAR             "); break;
        case OP_MINUS:                  fprintf(out, "OP_MINUS            "); break;
        case OP_GT:                     fprintf(out, "OP_GT               "); break;
        case OP_ARROW:                  fprintf(out, "OP_ARROW            "); break;
        default: break;
        }
        fprintf(out, "| Lexema: %.*s   ", (int)type.value.len, type.value.start);
        fprintf(out, "| Line: %ld | Col: %ld\n", type.line, type.col);
    }

    free(src);
    fclose(out);
}

int main(int argc, char* argv[]) {
    out_lexer(argv[1]);
}
