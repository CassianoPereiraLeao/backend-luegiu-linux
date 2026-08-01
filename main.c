#include "common.h"
#include "./lex/lex.h"
#include "./parser/parser.h"
#include "./parser/typecheck.h"
#include "./arena/arena.h"
#include "./diagnostics/diagnostics.h"
#include "./ir/ir.h"

#ifdef _WIN32
    #include "./codegen/windows/codegen.h"
#elif defined(__linux__)
    #include "./codegen/linux/codegen.h"
#endif

static void print_view(FILE *out, View view);

typedef struct {
    bool lex;
    bool parse;
    bool check;
    bool ir;
    bool codegen;
    const char* path;
} CliOptions;

static const char* severity_name(DiagSeverity s) {
    switch(s) {
        case NOTE:    return "nota";
        case WARNING: return "aviso";
        case ERROR:   return "erro";
        case FATAL:   return "fatal";
        default:      return "?";
    }
}

static const char* diag_type_name(DiagType t) {
    switch(t) {
        case LEXICAL:    return "lexico";
        case SYNTAX:     return "sintatico";
        case SEMANTICAL: return "semantico";
        case PREPROCESS: return "preprocessador";
        case INTERNAL:   return "interno";
        default:         return "?";
    }
}

static void diag_dump_to_file(DiagContext* context, FILE* out) {
    for(Diagnostic* d = context->first; d != NULL; d = d->next) {
        fprintf(out, "[%s/%s] %s:%zu:%zu: %s",
            severity_name(d->severity), diag_type_name(d->type),
            d->locale.filename ? d->locale.filename : "?",
            d->locale.line, d->locale.col,
            d->message ? d->message : "");
        if(d->code) fprintf(out, " (%s)", d->code);
        fprintf(out, "\n");
        if(d->src_line) fprintf(out, "    %s\n", d->src_line);
    }
}

static char* read_file(const char* path, size_t* out) {
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

    if((size_t)tell < bytes) {
        fprintf(stderr, "Falha ao ler arquivo alvo\n");
        exit(6);
    }

    fclose(f);

    src[bytes] = '\0';
    *out = bytes;
    return src;
}

static const char* token_type_name(TokenType type) {
    switch(type) {
        case KVOID:        return "VOID";
        case IDENTIFIER:   return "IDENTIFIER";
        case OPEN_PAREN:   return "OPEN_PAREN";
        case CLOSE_PAREN:  return "CLOSE_PAREN";
        case OPEN_BRACKET: return "OPEN_BRACKET";
        case CLOSE_BRACKET:return "CLOSE_BRACKET";
        case OPEN_BRACE:   return "OPEN_BRACE";
        case CLOSE_BRACE:  return "CLOSE_BRACE";
        case INT_LIT:      return "INT_LIT";
        case KINT8:        return "INT8";
        case KINT16:       return "INT16";
        case KINT32:       return "INT32";
        case KINT64:       return "INT64";
        case KUINT8:       return "UINT8";
        case KUINT16:      return "UINT16";
        case KUINT32:      return "UINT32";
        case KUINT64:      return "UINT64";
        case KBIG:         return "BIG";
        case KSMALL:       return "SMALL";
        case KCALL:        return "CALL";
        case UNDEFINED:    return "UNDEFINED";
        case SEMICOLON:    return "SEMICOLON";
        case COLON:        return "COLON";
        case DOT:          return "DOT";
        case ELLIPSES:     return "ELLIPSES";
        case KCHAR:        return "CHAR";
        case CHAR_LIT:     return "CHAR_LIT";
        case KDOUBLE:      return "DOUBLE";
        case DOUBLE_LIT:   return "DOUBLE_LIT";
        case KHEXA:        return "HEXA";
        case HEXA_LIT:     return "HEXA_LIT";
        case KFLOAT:       return "FLOAT";
        case FLOAT_LIT:    return "FLOAT_LIT";
        case STRING_LIT:   return "STRING_LIT";
        case COMMA:        return "COMMA";
        case OP_LSHIFT:    return "OP_LSHIFT";
        case OP_RSHIFT:    return "OP_RSHIFT";
        case OP_LSHIFTEQ:  return "OP_LSHIFTEQ";
        case OP_RSHIFTEQ:  return "OP_RSHIFTEQ";
        case KCOPERATE:    return "COPERATE";
        case KDATA:        return "DATA";
        case KNEWTYPE:     return "NEWTYPE";
        case KLINK:        return "LINK";
        case OP_ASSIGN:    return "OP_ASSIGN";
        case OP_EQUALS:    return "OP_EQUALS";
        case OP_STAR:      return "OP_STAR";
        case OP_MINUS:     return "OP_MINUS";
        case OP_GT:        return "OP_GT";
        case OP_ARROW:     return "OP_ARROW";
        case KWHILE:       return "WHILE";
        case KFOR:         return "FOR";
        case KIF:          return "IF";
        case OP_LT:        return "OP_LT";
        case OP_LE:        return "OP_GT";
        case KENUM:        return "ENUM";
        case KELSE:        return "ELSE";
        case KSTATIC:      return "STATIC";
        case KCONST:       return "CONST";
        default:           return "???";
    }
}

static void print_type_spec(FILE *out, TypeSpec spec) {
    if(spec.base == IDENTIFIER) {
        if(spec.nested)
            fprintf(out, "<inline aggregate>");
        else
            print_view(out, spec.name);
    } 
    else fprintf(out, "%s", token_type_name(spec.base));

    for(size_t i = 0; i < spec.ptr_lvl; ++i) fprintf(out, "*");
}

static void run_lex_dump(const char* src, const char* path, DiagContext *ctx) {
    FILE* out = fopen("./debug/exit.luegiu.lex", "wb");
    if(!out) {
        fprintf(stderr, "Nao foi possivel abrir ./debug/exit.luegiu.lex para escrita\n");
        return;
    }

    Lexer lexer = create_lexer(src, path, ctx);
    
    fprintf(out, "Analise lexica:\n\n");

    Token token;

    while((token = next_token(&lexer)).type != EOFF) {
        fprintf(out, "Token: %-20s| Lexema: %.*s   | Line: %ld | Col: %ld\n",
            token_type_name(token.type), (int)token.value.len, token.value.start, token.line, token.col);
    }

    fclose(out);
}

static void indent(FILE *out, int depth) {
    for(int i = 0; i < depth; ++i) fprintf(out, "  ");
}

static void print_view(FILE *out, View view) {
    fprintf(out, "%.*s", (int)view.len, view.start);
}

static void print_node(FILE *out, Node *node, int depth);

static void print_node_list(FILE *out, NodeList *list, int depth) {
    if(!list) return;
    for(size_t i = 0; i < list->count; ++i) {
        print_node(out, list->items[i], depth);
    }
}

static void print_node(FILE *out, Node *node, int depth) {
    if(!node) {
        indent(out, depth);
        fprintf(out, "<null>\n");
        return;
    }

    indent(out, depth);

    switch (node->kind)
    {
        case NODE_PROGRAM:
            fprintf(out, "PROGRAM [%s:%zu:%zu]\n", node->filename, node->line, node->col);
            print_node_list(out, node->ast.program.statements, depth + 1);
            break;

        case NODE_BLOCK:
            fprintf(out, "BLOCK [%zu:%zu]\n", node->line, node->col);
            print_node_list(out, node->ast.program.statements, depth + 1);
            break;

        case NODE_VAR_DECL:
            fprintf(out, "VarDecl name=");
            print_view(out, node->ast.decl_variable.name);
            fprintf(out, " type=");
            print_type_spec(out, node->ast.decl_variable.type);
            fprintf(out, " const=%d static=%d [%zu:%zu]\n",
                node->ast.decl_variable.constant,
                node->ast.decl_variable.stattic,
                node->line, node->col);
            if(node->ast.decl_variable.init) {
                indent(out, depth + 1); fprintf(out, "init:\n");
                print_node(out, node->ast.decl_variable.init, depth + 2);
            }
            break;

        case NODE_VAR_ACCESS:
            fprintf(out, "VAR ACCESS name=");
            print_view(out, node->ast.access_variable.name);
            fprintf(out, " [%zu:%zu]\n", node->line, node->col);
            break;

        case NODE_FUNC_DECL:
            fprintf(out, "FUNCTION DECLARATION name=");
            print_view(out, node->ast.decl_function.name);
            fprintf(out, " call_type=");
            print_type_spec(out, node->ast.decl_function.call_type);
            fprintf(out, " variadic=%d static=%d [%zu:%zu]\n",
                node->ast.decl_function.variadic,
                node->ast.decl_function.stattic,
                node->line, node->col);
            indent(out, depth + 1);
            fprintf(out, "params:\n");
            print_node_list(out, node->ast.decl_function.params, depth + 2);

            if(node->ast.decl_function.body) {
                indent(out, depth + 1);
                fprintf(out, "body:\n");
                print_node(out, node->ast.decl_function.body, depth + 2);
            }
            break;

        case NODE_ENUM:
            fprintf(out, "ENUM name=");
            print_view(out, node->ast.enum_decl.name);
            fprintf(out, " [%zu:%zu]", node->line, node->col);
            indent(out, depth + 1);
            fprintf(out, "members:");
            print_node_list(out, node->ast.enum_decl.members, depth + 1);
            break;

        case NODE_ENUM_MEMBER:
            fprintf(out, "ENUM MEMBER name=");
            print_view(out, node->ast.enum_member.name);
            fprintf(out, " value=");
            print_node(out, node->ast.enum_member.value, depth);
            break;

        case NODE_ENUM_ACCESS:
            fprintf(out, "ENUM ACCESS");
            break;

        case NODE_CALL_STMT:
            fprintf(out, "CALL [%zu:%zu]\n", node->line, node->col);
            if(node->ast.call_stmt.value) print_node(out, node->ast.call_stmt.value, depth + 1);
            break;

        case NODE_IF_STMT:
            fprintf(out, "IF ELSE STATMENT [%zu:%zu]\n", node->line, node->col);
            indent(out, depth + 1);
            fprintf(out, "condition:\n");
            print_node(out, node->ast.if_stmt.condition, depth + 2);
            indent(out, depth + 1);
            fprintf(out, "then:\n");
            print_node(out, node->ast.if_stmt.then, depth + 2);
            if(node->ast.if_stmt.otherwise) {
                indent(out, depth + 1);
                fprintf(out, "else:\n");
                print_node(out, node->ast.if_stmt.otherwise, depth + 2);
            }
            break;

        case NODE_WHILE_LOOP:
            fprintf(out, "WHILE LOOP [%zu:%zu]\n", node->line, node->col);
            indent(out, depth + 1);
            fprintf(out, "condition:\n");
            print_node(out, node->ast.while_loop.condition, depth + 2);
            indent(out, depth + 1);
            fprintf(out, "body:\n");
            print_node(out, node->ast.while_loop.body, depth + 2);
            break;

        case NODE_FOR_LOOP:
            fprintf(out, "FOR LOOP [%zu:%zu]\n", node->line, node->col);
            if(node->ast.for_loop.init) {
                indent(out, depth + 1);
                fprintf(out, "init:\n");
                print_node(out, node->ast.for_loop.init, depth + 2);
            }

            if(node->ast.for_loop.condition) {
                indent(out, depth + 1);
                fprintf(out, "condition:\n");
                print_node(out, node->ast.for_loop.condition, depth + 2);
            }

            if(node->ast.for_loop.increment) {
                indent(out, depth + 1);
                fprintf(out, "increment:\n");
                print_node(out, node->ast.for_loop.increment, depth + 2);
            }

            indent(out, depth + 1);
            fprintf(out, "body:\n");
            print_node(out, node->ast.for_loop.body, depth + 2);
            break;

        case NODE_BREAK_STMT:
            fprintf(out, "BREAK [%zu:%zu]\n", node->line, node->col);
            break;

        case NODE_CONTINUE_STMT:
            fprintf(out, "CONTINUE [%zu:%zu]\n", node->line, node->col);
            break;

        case NODE_BINARY_OP:
            fprintf(out, "BINARY OPERATOR op=%s [%zu:%zu]\n",
                token_type_name(node->ast.binary_operator.op), node->line, node->col);
            print_node(out, node->ast.binary_operator.left, depth + 1);
            print_node(out, node->ast.binary_operator.right, depth + 1);
            break;

        case NODE_UNARY_OP:
        case NODE_POSTFIX_OP:
            fprintf(out, "%s op=%s [%zu:%zu]\n",
                node->kind == NODE_UNARY_OP ? "UNARY OPERATOR" : "POSTFIX OPERATOR",
                token_type_name(node->ast.unary_operator.op), node->line, node->col);
            print_node(out, node->ast.unary_operator.operand, depth + 1);
            break;

        case NODE_ARRAY:
            fprintf(out, "ARRAY ACCESS [%zu:%zu]\n", node->line, node->col);
            print_node(out, node->ast.binary_operator.left, depth + 1);
            print_node(out, node->ast.binary_operator.right, depth + 1);
            break;

        case NODE_LITERAL:
            fprintf(out, "LITERAL ");
            switch (node->ast.literals.type)
            {
            case INT: fprintf(out, "int=%lld", node->ast.literals.integer64); break;
            case HEXA: fprintf(out, "hexa=0x%llx", node->ast.literals.integer64); break;
            case DOUBLE: fprintf(out, "double=%lf", node->ast.literals.double64); break;
            case FLOAT: fprintf(out, "float=%lf", node->ast.literals.double64); break;
            case CHAR: fprintf(out, "char="); print_view(out, node->ast.literals.character); break;
            case STRING: fprintf(out, "string="); print_view(out, node->ast.literals.string); break;
            }
            fprintf(out, " [%zu:%zu]\n", node->line, node->col);
            break;

        case NODE_DATA:
        case NODE_COPERATE:
            fprintf(out, "%s name=", node->kind == NODE_DATA ? "DATA" : "COPERATE");
            if(node->ast.aggregate.name.len > 0) print_view(out, node->ast.aggregate.name);
            else fprintf(out, "<anonimo>");
            fprintf(out, " [%zu:%zu]\n", node->line, node->col);

            if(node->ast.aggregate.members) {
                indent(out, depth + 1);
                fprintf(out, "members:\n");
                print_node_list(out, node->ast.aggregate.members, depth + 2);
            } else {
                indent(out, depth + 1);
                fprintf(out, "members: <referencia sem corpo>\n");
            }

            if(node->ast.aggregate.tailing_decl) {
                indent(out, depth + 1);
                fprintf(out, "trailing_decl:\n");
                print_node(out, node->ast.aggregate.tailing_decl, depth + 2);
            }

            break;

        case NODE_NEWTYPE:
            fprintf(out, "NEWTYPE name=");
            print_view(out, node->ast.newtype.name);
            fprintf(out, " underlying=");
            print_type_spec(out, node->ast.newtype.underlying);
            fprintf(out, " [%zu:%zu]\n", node->line, node->col);
            break;

        default:
            fprintf(out, "<node kind=%d nao suportado pelo dump> [%zu:%zu]\n",
                node->kind, node->line, node->col);
            break;
    }
}

static Node* run_parse(const char* src, const char* path, Arena *arena, DiagContext *context) {
    Lexer lexer = create_lexer(src, path, context);
    Parser parser = create_parser(&lexer, arena, context);
    return parse_program(&parser);
}

static void run_parse_dump(Node *program) {
    FILE* out = fopen("./debug/exit.luegiu.parser", "wb");
    if(!out) {
        fprintf(stderr, "Nao foi possivel abrir ./debug/exit.luegiu.parser para escrita\n");
        return;
    }

    fprintf(out, "Analise Sintatica:\n\n");
    print_node(out, program, 0);

    fclose(out);
}

static void run_check_dump(CheckContext *ctx, Node *program, DiagContext *context) {
    FILE* out = fopen("./debug/exit.luegiu.checker", "wb");
    if(!out) {
        fprintf(stderr, "Nao foi possivel abrir ./debug/exit.luegiu.checker para escrita\n");
        return;
    }

    check_program(ctx, program);

    fprintf(out, "Analise Semantica:\n\n");
    fprintf(out, "Erros: %zu\n", diag_error_count(context));
    fprintf(out, "Warnings: %zu\n", diag_warning_count(context));
    fprintf(out, "\n--- Diagnosticos ---\n");
    diag_dump_to_file(context, out);

    fclose(out);
}

static const char* ir_op_name(IrOperators op) {
    switch (op)
    {
    case IR_ADD: return "add";
    case IR_SUB: return "sub";
    case IR_MUL: return "mul";
    case IR_DIV: return "div";
    case IR_MOD: return "mod";
    case IR_BAND: return "and";
    case IR_BOR: return "or";
    case IR_BXOR: return "xor";
    case IR_SHL: return "shl";
    case IR_SHR: return "shr";
    case IR_NEG: return "neg";
    case IR_NOT: return "not";
    case IR_CMP_LT: return "lt";
    case IR_CMP_LE: return "le";
    case IR_CMP_GT: return "gt";
    case IR_CMP_GE: return "ge";
    case IR_CMP_EQ: return "eq";
    case IR_CMP_NE: return "ne";
    case IR_ASSIGN: return "assign";
    case IR_LABEL: return "label";
    case IR_JMP: return "jmp";
    case IR_JMP_IF_ZERO: return "jz";
    default: return "error";
    }
}

static void print_ir_value(FILE *out, IrValue value) {
    switch (value.kind)
    {
    case IR_VAL_TEMP:           fprintf(out, "t%d", value.as.temp_id);  break;
    case IR_VAL_SLOT:           fprintf(out, "s%d", value.as.slot_id);  break;
    case IR_VAL_LABEL:          fprintf(out, "l%d", value.as.label_id); break;
    case IR_VAL_CONST_INT:      fprintf(out, "%lld", value.as.const_i); break;
    case IR_VAL_CONST_FLOAT:    fprintf(out, "%lf", value.as.const_f);  break;
    case IR_VAL_NONE:           fprintf(out, "-");                      break;
    default:
        break;
    }
}

static void run_ir_dump(Node *program, Arena *arena) {
    FILE* out = fopen("./debug/exit.luegiu.ir", "wb");
    if(!out) {
        fprintf(stderr, "Nao foi possivel abrir ./debug/exit.luegiu.ir para escrita\n");
        return;
    }

    IrGenContext ctx = create_irgen_context(arena);
    irgen_program(&ctx, program);

    fprintf(out, "Representacao Intemediaria:\n\n");

    for(size_t i = 0; i < ctx.instructions.count; ++i) {
        IrInstruction instruction = ctx.instructions.items[i];

        if(instruction.op == IR_LABEL) {
            print_ir_value(out, instruction.dest);
            fprintf(out, ":\n");
            continue;
        }

        if(instruction.op == IR_JMP) {
            fprintf(out, "    jmp ");
            print_ir_value(out, instruction.src1);
            fprintf(out, "\n");
            continue;
        }

        if(instruction.op == IR_JMP_IF_ZERO) {
            fprintf(out, "    jz ");
            print_ir_value(out, instruction.src1);
            fprintf(out, ", ");
            print_ir_value(out, instruction.src2);
            fprintf(out, "\n");
            continue;
        }

        fprintf(out, "    ");
        print_ir_value(out, instruction.dest);
        fprintf(out, " = ");
        if(instruction.op == IR_ASSIGN) {
            print_ir_value(out, instruction.src1);
        } else {
            print_ir_value(out, instruction.src1);
            fprintf(out, " %s ", ir_op_name(instruction.op));
            print_ir_value(out, instruction.src2);
        }

        fprintf(out, "\n");
    }

    fclose(out);
}

static void run_codegen_dump(IrGenContext *ctx, Arena *arena, Node *entry) {
    FILE* out = fopen("./debug/out.s", "wb");
    if(!out) {
        fprintf(stderr, "Nao foi possivel abrir ./debug/out.asm para escrita\n");
        return;
    }

    CodegenContext context = create_codegen(out, arena, entry);
    
    emit_program(&context, ctx);

    fclose(out);
}

static CliOptions parse_args(int argc, char* argv[]) {
    CliOptions opts = { 0 };

    for(int i = 1; i < argc; ++i) {
        if(strcmp(argv[i], "-Lex") == 0) opts.lex = true;
        else if(strcmp(argv[i], "-Parse") == 0) opts.parse = true;
        else if(strcmp(argv[i], "-Check") == 0) opts.check = true;
        else if(strcmp(argv[i], "-Ir") == 0) opts.ir = true;
        else if(strcmp(argv[i], "-Codegen") == 0) opts.codegen = true;
        else if(!opts.path) opts.path = argv[i];
    }
    return opts;
}

int main(int argc, char* argv[]) {
    CliOptions opts = parse_args(argc, argv);

    if(!opts.path) {
        fprintf(stderr, "Uso: %s [-Lex] [-Parse] [-Check] <arquivo>\n", argv[0]);
        return 1;
    }

    if(!opts.lex && !opts.parse && !opts.check) {
        fprintf(stderr, "Nenhuma flag informada. Use -Lex, -Parse e/ou -Check.\n");
        return 1;
    }

    size_t size;
    char* src = read_file(opts.path, &size);

    Arena arena;
    arena_init(&arena, 1024 * 1024);

    DiagContext context;
    diag_init(&context, &arena);
    diag_set_max_errors(&context, 200);

    if(opts.lex) 
        run_lex_dump(src, opts.path, &context);

    Node* program = NULL;
    if(opts.parse || opts.check) {
        program = run_parse(src, opts.path, &arena, &context);

        if(has_error(&context)) {
            diag_report_all(&context);
            free(src);
            arena_free(&arena);
            return 1;
        }
    }

    CheckContext check_ctx = create_check_context(&arena, &context);

    if(opts.parse) {
        run_parse_dump(program);
    }

    if(opts.check) {
        run_check_dump(&check_ctx, program, &context);
    }

    if(opts.ir) {
        if(has_error(&context)) return 1;
        else run_ir_dump(program, &arena);
    }

    if(opts.codegen) {
        IrGenContext ir_ctx = create_irgen_context(&arena);
        irgen_program(&ir_ctx, program);
        run_codegen_dump(&ir_ctx, &arena, check_ctx.entry_function);
    }

    free(src);
    arena_free(&arena);
    return 0;
}
