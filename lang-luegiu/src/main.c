#include "../include/string_view_helper.h"
#include "../include/preprocess.h"
#include "../include/diagnostic.h"
#include "../include/typecheck.h"
#include "../include/codegen.h"
#include "../include/common.h"
#include "../include/parser.h"
#include "../include/arena.h"
#include "../include/lexer.h"
#include "../include/ir.h"
#include "helpers/exitcodes.h"

#define MAX_POSSIBLE_PATHS 256

typedef struct {
    FILE* out;
    const char* path;
    const char* filename;
} StreamFile;

typedef enum {
    PRIO_BP,
    PRIO_PREPROCESS,
    PRIO_LEXER,
    PRIO_PARSER,
    PRIO_CHECK,
    PRIO_IR,
    PRIO_GEN
} PathNamePriority;

typedef struct {
    char* resolved[MAX_POSSIBLE_PATHS];
    uint16_t count;
    char* out_path;
} PathsResolve;

typedef struct {
    bool before_process;
    bool preprocess;
    bool lex;
    bool parse;
    bool check;
    bool ir;
    bool assembly;
    PathsResolve paths;
    PathNamePriority priority;
} GenerateDebugOptions;

static void register_source_cb(void *userdata, const char* filename, const char* src) {
    diag_register_source((DiagContext*)userdata, filename, src);
}

static GenerateDebugOptions shift(int argc, char** argv) {
    GenerateDebugOptions options = { 0 };
    options.paths.count = 0;
    options.paths.out_path = "./out.asm";
    PathNamePriority priority = PRIO_BP;

    for(int i = 1; i < argc; ++i) {
        if(strcmp(argv[i], "-bp") == 0) {
            options.before_process = true;
            priority = (priority > PRIO_BP) ? priority : PRIO_BP;
        } else if(strcmp(argv[i], "-pp") == 0) {
            options.preprocess = true;
            priority = (priority > PRIO_PREPROCESS) ? priority : PRIO_PREPROCESS;
        } else if(strcmp(argv[i], "-lex") == 0) {
            options.lex = true;
            priority = (priority > PRIO_LEXER) ? priority : PRIO_LEXER;
        } else if(strcmp(argv[i], "-parse") == 0) { 
            options.parse = true; 
            priority = (priority > PRIO_PARSER) ? priority : PRIO_PARSER;
        } else if(strcmp(argv[i], "-check") == 0) {
            options.check = true;
            priority = (priority > PRIO_CHECK) ? priority : PRIO_CHECK;
        } else if(strcmp(argv[i], "-ir") == 0) {
            options.ir = true;
            priority = (priority > PRIO_IR) ? priority : PRIO_IR;
        } else if(strcmp(argv[i], "-codegen") == 0) {
            options.assembly = true;
            priority = PRIO_GEN;
        } else if(strcmp(argv[i], "-out") == 0) {
            options.paths.out_path = argv[i + 1];
            ++i;
            continue;
        }
        else options.paths.resolved[options.paths.count++] = argv[i];
    }

    options.priority = priority;
    return options;
}

static const char* prio_to_str(PathNamePriority priority) {
    switch (priority)
    {
    case PRIO_BP: return "before_process";
    case PRIO_PREPROCESS: return "preprocess";
    case PRIO_LEXER: return "lex";
    case PRIO_PARSER: return "parse";
    case PRIO_CHECK: return "check";
    case PRIO_IR: return "ir";
    case PRIO_GEN: return "codegen";
    }

    return "dump";
}

static void indent(FILE *out, int depth) {
    for(int i = 0; i < depth; ++i) fprintf(out, "  ");
}

static void print_view(FILE *out, View view) {
    fprintf(out, "%.*s", (int)view.len, view.start);
}

static void print_node(FILE *out, Node *node, int depth);

static void print_nodelist(FILE *out, NodeList *list, int depth) {
    if(!list) return;

    for(size_t i = 0; i < list->count; ++i) {
        print_node(out, list->items[i], depth);
    }
}

static void print_type_spec(FILE* out, TypeSpec spec) {
    if(spec.base == IDENTIFIER) {
        if(spec.nested)
            fprintf(out, "<inline aggregate>");
        else
            print_view(out, spec.alias);
    } else {
        fprintf(out, "%s", tk_to_str(spec.base));
    }

    for(uint8_t i = 0; i < spec.ptr_lvl; ++i) fprintf(out, "*");
}

static const char* bool_to_str(bool istrue) {
    if(istrue) return "true";

    return "false";
}

static void print_node(FILE *out, Node *node, int depth) {
    if(!node) {
        indent(out, depth);
        fprintf(out, "<null>"); NL;
        return;
    }

    indent(out, depth);

    switch (node->kind)
    {
    case NODE_PROGRAM:
        fprintf(out, "PROGRAM [%s:%zu:%zu]", node->filename, node->line, node->col); NL;
        print_nodelist(out, node->ast.program.statements, depth + 1);
        break;

    case NODE_BLOCK_STMT:
        fprintf(out, "BLOCK [%zu:%zu]", node->line, node->col); NL;
        print_nodelist(out, node->ast.program.statements, depth + 1);
        break;

    case NODE_VAR_DECL:
        const char* var_name = tk_to_str(node->ast.decl_variable.call_type.base);
        fprintf(out, "VAR DECLARATION [%zu:%zu]", node->line, node->col); NL;
        indent(out, depth); fprintf(out, " name = "); print_view(out, node->ast.decl_variable.name); NL;
        indent(out, depth); fprintf(out, " type = "); print_view(out, (View){ var_name, strlen(var_name) }); NL;
        indent(out, depth); fprintf(out, " const = %s", bool_to_str(node->ast.decl_variable._const)); NL;
        indent(out, depth); fprintf(out, " atomic = %s", bool_to_str(node->ast.decl_variable._atomic)); NL;
        indent(out, depth); fprintf(out, " static = %s", bool_to_str(node->ast.decl_variable._static)); NL;
        indent(out, depth); fprintf(out, " extern = %s", bool_to_str(node->ast.decl_variable._extern)); NL;
        indent(out, depth); fprintf(out, " pointer_lvl = %d", node->ast.decl_variable.call_type.ptr_lvl); NL;

        if(node->ast.decl_variable.init) {
            indent(out, depth); fprintf(out, " init:"); NL;
            print_node(out, node->ast.decl_variable.init, depth + 1);
        }
        NL;
        break;

    case NODE_VAR_CALL:
        fprintf(out, "VAR CALL [%zu:%zu]", node->line, node->col); NL;
        indent(out, depth); fprintf(out, " name = "); print_view(out, node->ast.call_variable.name); NL;
        break;

    case NODE_CAST:
        fprintf(out, "CAST [%zu:%zu]", node->line, node->col); NL;
        indent(out, depth); fprintf(out, " cast for = "); print_type_spec(out, node->ast.cast.spec); NL;
        break;

    case NODE_BINARY_OP:
        fprintf(out, "BINARY OP [%zu:%zu]", node->line, node->col); NL;
        indent(out, depth); fprintf(out, " operator = %s", tk_to_str(node->ast.binary.op)); NL;
        print_node(out, node->ast.binary.left, depth + 1);
        print_node(out, node->ast.binary.right, depth + 1);
        break;

    case NODE_BREAK_STMT:
        fprintf(out, "BREAK [%zu:%zu]", node->line, node->col); NL;
        break;

    case NODE_CONTINUE_STMT:
        fprintf(out, "CONTINUE [%zu:%zu]", node->line, node->col); NL;
        break;

    case NODE_LITERAL:
        fprintf(out, "LITERAL [%zu:%zu]", node->line, node->col); NL;
        indent(out, depth);

        switch (node->ast.literal.kind)
        {
        case STRING: fprintf(out, " string = "); print_view(out, node->ast.literal.string); break;
        case CHAR: fprintf(out, " char = "); print_view(out, node->ast.literal.character); break;
        case HEXA: fprintf(out, " hexa = %zu", node->ast.literal.unsigned64); break;
        case UNSIGNED: fprintf(out, " uint = %zu", node->ast.literal.unsigned64); break;
        case INT: fprintf(out, " int = %lld", node->ast.literal.integer64); break;
        case BIG: fprintf(out, " big = %lld", node->ast.literal.integer64); break;
        case FLOAT: fprintf(out, " float = %Lf", node->ast.literal.double64); break;
        case DOUBLE: fprintf(out, " double = %Lf", node->ast.literal.double64); break;
        }
        NL;
        break;

    case NODE_CALL_STMT: 
        fprintf(out, "CALL [%zu:%zu]", node->line, node->col); NL;
        if(node->ast.call_stmt.value != NULL) print_node(out, node->ast.call_stmt.value, depth + 1);
        break;

    case NODE_FUNC_DECL:
        fprintf(out, "FUNC DECLARATION [%zu:%zu]", node->line, node->col); NL;
        indent(out, depth); fprintf(out, " name = "); print_view(out, node->ast.decl_function.name); NL;
        indent(out, depth); fprintf(out, " call type = "); print_type_spec(out, node->ast.decl_function.call_type); NL;
        indent(out, depth); fprintf(out, " atomic = %s", bool_to_str(node->ast.decl_function._atomic)); NL;
        indent(out, depth); fprintf(out, " static = %s", bool_to_str(node->ast.decl_function._static)); NL;
        indent(out, depth); fprintf(out, " variadic = %s", bool_to_str(node->ast.decl_function._variadic)); NL;
        indent(out, depth); fprintf(out, " const = %s", bool_to_str(node->ast.decl_function._const)); NL;
        indent(out, depth); fprintf(out, " extern = %s", bool_to_str(node->ast.decl_function._extern)); NL;
        indent(out, depth); fprintf(out, " params:"); NL;
        print_nodelist(out, node->ast.decl_function.params, depth + 1);

        if(node->ast.decl_function.body) {
            indent(out, depth);
            fprintf(out, " body:"); NL;
            print_node(out, node->ast.decl_function.body, depth + 1);
        }
        NL;
        break;
    
    case NODE_FUNC_CALL:
        fprintf(out, "FUNC CALL [%zu:%zu]", node->line, node->col); NL;
        indent(out, depth); fprintf(out, " name = "); print_view(out, node->ast.call_function.name); NL;
        if(node->ast.call_function.args) print_nodelist(out, node->ast.call_function.args, depth + 1);
        NL;
        break;

    case NODE_WHILE_LOOP:
        fprintf(out, "WHILE [%zu:%zu]", node->line, node->col); NL;
        indent(out, depth); fprintf(out, " expr:"); NL;
        print_node(out, node->ast.while_loop.condition, depth + 1);

        if(node->ast.while_loop.body) {
            indent(out, depth);
            fprintf(out, " body:"); NL;
            print_node(out, node->ast.while_loop.body, depth + 1);
        }
        NL;
        break;

    case NODE_DO_WHILE_LOOP:
        fprintf(out, "DO WHILE [%zu:%zu]", node->line, node->col); NL;
        indent(out, depth); fprintf(out, " body:"); NL;
        print_node(out, node->ast.while_loop.body, depth + 1);
        indent(out, depth); fprintf(out, " expr:"); NL;
        print_node(out, node->ast.while_loop.condition, depth + 1);
        NL;
        break;

    case NODE_FOR_LOOP:
        fprintf(out, "FOR [%zu:%zu]", node->line, node->col); NL;
        
        if(node->ast.for_loop.init) {
            indent(out, depth); fprintf(out, " init:"); NL;
            print_node(out, node->ast.for_loop.init, depth + 1);
        }

        if(node->ast.for_loop.condition) {
            indent(out, depth); fprintf(out, " expr:"); NL;
            print_node(out, node->ast.for_loop.condition, depth + 1);
        }

        if(node->ast.for_loop.increment) {
            indent(out, depth); fprintf(out, " increment:"); NL;
            print_node(out, node->ast.for_loop.increment, depth + 1);
        }

        if(node->ast.for_loop.body) {
            indent(out, depth); fprintf(out, " body:"); NL;
            print_node(out, node->ast.for_loop.body, depth + 1);
        }
        NL;
        break;

    case NODE_DATA:
        fprintf(out, "DATA [%zu:%zu]", node->line, node->col); NL;
        print_nodelist(out, node->ast.aggregate.members, depth + 1);
        NL;
        break;

    case NODE_COPERATE:
        fprintf(out, "COPERATE [%zu:%zu]", node->line, node->col); NL;
        print_nodelist(out, node->ast.aggregate.members, depth + 1);
        NL;
        break;

    case NODE_ENUM:
        fprintf(out, "ENUM [%zu:%zu]", node->line, node->col); NL;
        indent(out, depth); fprintf(out, " members:"); NL;
        print_nodelist(out, node->ast.enum_decl.members, depth + 1);
        NL;
        break;

    case NODE_ENUM_MEMBER:
        fprintf(out, "ENUM MEMBER [%zu:%zu]", node->line, node->col); NL;
        indent(out, depth); fprintf(out, " name = "); print_view(out, node->ast.enum_member.name); NL;
        indent(out, depth); fprintf(out, " value:"); NL;
        print_node(out, node->ast.enum_member.value, depth + 1);
        break;

    case NODE_ENUM_CALL:
        fprintf(out, "ENUM CALL [%zu:%zu]", node->line, node->col); NL;
        indent(out, depth); fprintf(out, " enum name = "); print_view(out, node->ast.enum_access.alias); NL;
        indent(out, depth); fprintf(out, " enum member = "); print_view(out, node->ast.enum_access.member); NL;
        break;

    case NODE_NEWTYPE:
        fprintf(out, "NEWTYPE [%zu:%zu]", node->line, node->col); NL;
        indent(out, depth); fprintf(out, " name = "); print_view(out, node->ast.newtype.alias); NL;
        indent(out, depth); fprintf(out, " spec:"); NL;
        indent(out, depth + 1); print_type_spec(out, node->ast.newtype.underlying); NL;
        NL;
        break;

    case NODE_FIELD_CALL:
        fprintf(out, "FIELD CALL [%zu:%zu]", node->line, node->col); NL;
        indent(out, depth); fprintf(out, " name = "); print_view(out, node->ast.field_access.name); NL;
        indent(out, depth); fprintf(out, " arrow = %s", bool_to_str(node->ast.field_access.arrow)); NL;
        indent(out, depth); fprintf(out, " base:"); NL;
        print_node(out, node->ast.field_access.base, depth + 1);
        NL;
        break;

    case NODE_IF_STMT:
        fprintf(out, "IF STATEMENT [%zu:%zu]", node->line, node->col); NL;
        indent(out, depth); fprintf(out, " condition:"); NL;
        print_node(out, node->ast.if_stmt.condition, depth + 1);
        indent(out, depth); fprintf(out, " then:"); NL;
        print_node(out, node->ast.if_stmt.then, depth + 1);
        if(node->ast.if_stmt.otherwise) {
            indent(out, depth); fprintf(out, " otherwise:"); NL;
            print_node(out, node->ast.if_stmt.otherwise, depth + 1);
        }
        NL;
        break;

    case NODE_JUMP_LABEL:
        fprintf(out, "JUMP [%zu:%zu]", node->line, node->col); NL;
        indent(out, depth); fprintf(out, " label_name = "); print_view(out, node->ast.jmp_label.name); NL;
        break;
    
    case NODE_LOAD_LABEL:
        fprintf(out, "LOAD [%zu:%zu]", node->line, node->col); NL;
        indent(out, depth); fprintf(out, " label_name = "); print_view(out, node->ast.jmp_label.name); NL;
        break;
    
    default:
        fprintf(stdout, "Ainda criando o caso para o debug, esperando manutencao: %s", node_to_str(node->kind)); NLSTDOUT;
        return;
    }
}

static char* sanitize_path(const char* path, char* out, size_t out_size) {
    size_t i = 0;
    size_t j = 0;
    
    while(path[i] != '\0' && j < out_size - 1) {
        if(path[i] == '.' && (path[i + 1] == '/' || path[i + 1] == '\\')) {
            ++i;
            continue;
        }

        char c = path[i++];
        out[j++] = (c == '/' || c == '\\') ? '_' : c;
    }

    out[j] = '\0';
    return out;
}

static void dump_lexer_analyse(Lexer *lexer, StreamFile stream, bool need_before_steps) {
    if(need_before_steps) stream.out = fopen(stream.path, "ab");
    FILE* out = stream.out;

    fprintf(out, "Analise lexica dos recursos:"); NL;
    
    Token token;
    while((token = next_token(lexer)).type != EOFF) {
        fprintf(out, "Token: %-20s | Lexema: %-10.*s | StringLen: %-3lld | Line: %-4lld | Col: %lld\n",
            tk_to_str(token.type), (int)token.value.len, token.value.start, token.value.len, token.line, token.col);
    }

    NL;

    fclose(out);
    out = NULL;
}

static void dump_before_preprocess_analyse(const char* src, StreamFile stream) {
    FILE* out = stream.out;

    fprintf(out, "Codigo antes do preprocessamento: "); NL;
    fprintf(out, "%s", src); NL;

    fclose(out);
    out = NULL;
}

static void dump_preprocess_analyse(const char* preprocessed, StreamFile stream, bool need_before_steps) {
    if(need_before_steps) stream.out = fopen(stream.path, "ab");
    FILE* out = stream.out;

    fprintf(out, "Codigo apos o preprocessamento: "); NL;
    fprintf(out, "%s", preprocessed); NL;

    fclose(out);
    out = NULL;
}

static void print_func_entry(FuncEntry *entry, void *userdata) {
    FILE *out = (FILE*)userdata;
    fprintf(out, "FUNC: "); print_view(out, entry->name); NL;
    fprintf(out, "  static = %s", bool_to_str(entry->_static)); NL;
    fprintf(out, "  extern = %s", bool_to_str(entry->_extern)); NL;
    fprintf(out, "  pending = %s", bool_to_str(entry->_pending)); NL;
    fprintf(out, "  has_body = %s", bool_to_str(entry->has_body)); NL;
    fprintf(out, "  param_count = %zu", entry->signature.param_count); NL;
    fprintf(out, "  variadic = %s", bool_to_str(entry->signature.variadic)); NL;
}

static void print_label_entry(LabelEntry *entry, void *userdata) {
    FILE *out = (FILE*)userdata;
    fprintf(out, "LABEL: "); print_view(out, entry->name); NL;
    fprintf(out, "  defined = %s", bool_to_str(entry->_defined)); NL;
}

static void dump_check_analyse(CheckContext *checkctx, Node *program, StreamFile stream, bool need_before_steps) {
    if(need_before_steps) {
        stream.out = fopen(stream.path, "ab");
    }

    FILE* out = stream.out;

    fprintf(out, "Analise semantica dos recursos:"); NL;

    check_program(checkctx, program);

    indent(out, 1); fprintf(out, "Funcoes:"); NL;
    function_table_foreach(print_func_entry, out);

    indent(out, 1); fprintf(out, "Labels:"); NL;
    label_table_foreach(print_label_entry, out);

    fprintf(out, "Erros: %zu | Warnings: %zu",
        diag_error_count(checkctx->ctx), diag_warning_count(checkctx->ctx)); NL;

    fclose(out);
    out = NULL;

    diag_report_all(checkctx->ctx);
}

static void dump_parser_analyse(Node* program, StreamFile stream, bool need_before_steps) {
    if(need_before_steps) { 
        stream.out = fopen(stream.path, "ab"); 
    }

    FILE* out = stream.out;

    fprintf(out, "Analise sintatica dos recursos:"); NL;
    print_node(out, program, 0);

    fclose(out);
    out = NULL;
}

static char* readfile(const char* path) {
    FILE* file = fopen(path, "rb");
    if(!file) {
        fprintf(stderr, "O arquivo passado ao compilador esta errado: %s\n", path);
        exit(WRONG_PATH);
    }

    fseek(file, 0L, SEEK_END);
    long tell = ftell(file);
    fseek(file, 0L, SEEK_SET);

    char* source = (char*)malloc(tell + 1);
    if(source == NULL) {
        fprintf(stderr, "Erro no malloc, tente novamente o comando\n");
        fclose(file);
        exit(MALLOC_ERROR);
    }

    size_t bytes = fread(source, 1, tell, file);
    if(bytes < (size_t)tell) {
        fprintf(stderr, "Os bytes do arquivo são maiores que os bytes lidos\n");
        free(source);
        fclose(file);
        exit(READFILE_ERROR);
    }
    source[bytes] = '\0';

    fclose(file);
    return source;
}

static void print_value(FILE* out, IrValue value) {
    switch (value.kind)
    {
    case IR_VAL_NONE:
        fprintf(out, "_ ");
        break;
    case IR_VAL_SLOT:
        fprintf(out, "S%d ", value.as.slot_id);
        break;
    case IR_VAL_TEMP:
        fprintf(out, "T%d ", value.as.temp_id);
        break;
    case IR_VAL_LABEL:
        fprintf(out, "L%d", value.as.label_id);
        break;
    case IR_VAL_CONST_INT:
        fprintf(out, "%lld ", value.as.const_i);
        break;
    case IR_VAL_CONST_UINT:
        fprintf(out, "%zu ", value.as.const_ui);
        break;
    case IR_VAL_CONST_FLOAT:
        fprintf(out, "%lf ", value.as.const_f);
        break;
    case IR_VAL_CONST_STRING:
        fprintf(out, "str#%d ", value.as.string_id);
        break;
    case IR_VAL_FUNC:
        if(value.func_name.start) 
            fprintf(out, "%.*s", (int)value.func_name.len, value.func_name.start);
        else
            fprintf(out, "func#%d ", value.as.func_id);
        break;
    case IR_VAL_BUILTIN:
        fprintf(out, "builtin#%d ", value.as.func_id);
        break;
    default:
        fprintf(out, "?");
        break;
    }

    if(value.field_offset != 0)
        fprintf(out, "+%d", value.field_offset);
}

static const char* binop_symbol(IrOperators op) {
    switch (op)
    {
    case IR_ADD: return "ADD ";
    case IR_SUB: return "SUB ";
    case IR_MUL: return "IMUL ";
    case IR_DIV:
    case IR_MOD: return "IDIV ";
    case IR_BAND: return "AND ";
    case IR_BOR: return "OR ";
    case IR_BXOR: return "XOR ";
    case IR_SHL: return "SHL ";
    case IR_SHR: return "SHR ";
    case IR_CMP_LT: return "cmp lt ";
    case IR_CMP_LE: return "cmp le ";
    case IR_CMP_GT: return "cmp gt ";
    case IR_CMP_GE: return "cmp ge ";
    case IR_CMP_EQ: return "cmp eq ";
    case IR_CMP_NE: return "cmp ne ";
    default: return "? ";
    }
}

void print_ir(FILE* out, IrInstruction instruction, int depth) {
    switch (instruction.op)
    {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_BAND: case IR_BOR: case IR_BXOR: case IR_SHL: case IR_SHR:
    case IR_CMP_LT: case IR_CMP_LE: case IR_CMP_GT: case IR_CMP_GE:
    case IR_CMP_EQ: case IR_CMP_NE:
        indent(out, depth + 1);
        print_value(out, instruction.dest);
        fprintf(out, "= ");
        print_value(out, instruction.src1);
        fprintf(out, "%s", binop_symbol(instruction.op));
        print_value(out, instruction.src2);
        NL;
        break;

    case IR_NEG:
        indent(out, depth + 1);
        print_value(out, instruction.dest);
        fprintf(out, "= ");
        print_value(out, instruction.src1);
        NL;
        break;

    case IR_NOT:
        indent(out, depth + 1);
        print_value(out, instruction.dest);
        fprintf(out, " = !");
        print_value(out, instruction.src1);
        NL;
        break;

    case IR_ASSIGN:
        indent(out, depth + 1);
        print_value(out, instruction.dest);
        fprintf(out, "= ");
        print_value(out, instruction.src1);
        NL;
        break;
    
    case IR_LABEL:
        print_value(out, instruction.dest);
        fprintf(out, ":");
        NL;
        break;

    case IR_JMP:
        indent(out, depth + 1);
        fprintf(out, "JMP ");
        print_value(out, instruction.src1);
        NL;
        break;

    case IR_JMP_IF_ZERO:
        indent(out, depth + 1);
        fprintf(out, "jz ");
        print_value(out, instruction.src1);
        fprintf(out, ", ");
        print_value(out, instruction.src2);
        NL;
        break;

    case IR_RETURN:
        indent(out, depth + 1);
        if(instruction.src1.kind == IR_VAL_NONE) {
            fprintf(out, "return"); NL;
        } else {
            fprintf(out, "return ");
            print_value(out, instruction.src1);
            NL;
        }
        break;

    case IR_ARG:
        indent(out, depth + 1);
        fprintf(out, "arg[%d] = ", instruction.aux);
        print_value(out, instruction.src1);
        NL;
        break;

    case IR_ARG_STACK:
        indent(out, depth + 1);
        fprintf(out, "arg stack ");
        print_value(out, instruction.src1);
        NL;
        break;

    case IR_CALL:
        indent(out, depth + 1);
        print_value(out, instruction.dest);
        fprintf(out, "= call ");
        print_value(out, instruction.src1);
        fprintf(out, ", argc = %d", instruction.aux);
        NL;
        break;
        
    case IR_SLOT_DECL:
        indent(out, depth + 1);
        fprintf(out, "slot decl ");
        print_value(out, instruction.dest);
        NL;
        break;

    case IR_LOAD_INDIRECT:
        indent(out, depth + 1);
        print_value(out, instruction.dest);
        fprintf(out, "= load [");
        print_value(out, instruction.src1);
        if(instruction.aux != 0) fprintf(out, "+ %d", instruction.aux);
        fprintf(out, "]");
        NL;
        break;

    case IR_STORE_INDIRECT:
        indent(out, depth + 1);
        fprintf(out, "store [");
        print_value(out, instruction.src1);
        if(instruction.aux != 0) fprintf(out, "+ %d", instruction.aux);
        fprintf(out, "] = ");
        print_value(out, instruction.src2);
        NL;
        break;

    case IR_ALLOCA:
        indent(out, depth + 1);
        print_value(out, instruction.dest);
        fprintf(out, "= alloca ");
        print_value(out, instruction.src1);
        NL;
        break;

    case IR_STACK_SAVE:
        indent(out, depth + 1);
        print_value(out, instruction.dest);
        fprintf(out, "= stack save");
        NL;
        break;

    case IR_STACK_RESTORE:
        indent(out, depth + 1);
        fprintf(out, "stack restore ");
        print_value(out, instruction.src1);
        NL;
        break;

    case IR_ATOMIC_ADD:
        indent(out, depth + 1);
        print_value(out, instruction.dest);
        fprintf(out, "= atomic add [");
        print_value(out, instruction.src1);
        if(instruction.aux != 0) fprintf(out, "+ %d", instruction.aux);
        fprintf(out, "], ");
        print_value(out, instruction.src2);
        NL;
        break;

    case IR_ATOMIC_AND:
    case IR_ATOMIC_OR:
    case IR_ATOMIC_XOR:
    case IR_ATOMIC_SHL:
    case IR_ATOMIC_SHR:
        const char* name =
            instruction.op == IR_ATOMIC_AND ? "atomic and" :
            instruction.op == IR_ATOMIC_OR  ? "atomic or"  :
            instruction.op == IR_ATOMIC_XOR ? "atomic xor" :
            instruction.op == IR_ATOMIC_SHL ? "atomic shl" : "atomic shr";

        indent(out, depth + 1);
        print_value(out, instruction.dest);
        fprintf(out, "= %s [", name);
        print_value(out, instruction.src1);
        if(instruction.aux != 0) fprintf(out, "+ %d", instruction.aux);
        fprintf(out, "], ");
        print_value(out, instruction.src2);
        NL;
        break;

    case IR_ATOMIC_CAS:
        indent(out, depth + 1);
        print_value(out, instruction.dest);
        fprintf(out, "= atomic cas [");
        print_value(out, instruction.src1);
        fprintf(out, "], expected = ");
        print_value(out, instruction.src2);
        fprintf(out, ", desired = ");
        print_value(out, instruction.src3);
        NL;
        break;

    case IR_ATOMIC_LOAD:
        indent(out, depth + 1);
        print_value(out, instruction.dest);
        fprintf(out, "= atomic load [");
        print_value(out, instruction.src1);
        fprintf(out, "]");
        NL;
        break;

    case IR_ATOMIC_STORE:
        indent(out, depth + 1);
        fprintf(out, "atomic store [");
        print_value(out, instruction.src1);
        fprintf(out, "] = ");
        print_value(out, instruction.src2);
        NL;
        break;

    case IR_ATOMIC_XCHG:
        indent(out, depth + 1);
        print_value(out, instruction.dest);
        fprintf(out, "= atomic xchg [");
        print_value(out, instruction.src1);
        fprintf(out, "], ");
        print_value(out, instruction.src2);
        NL;
        break;

    case IR_FENCE:
        indent(out, depth + 1);
        fprintf(out, "fence");
        NL;
        break;

    default:
        indent(out, depth);
        fprintf(out, "<op desconhecido: %d>", (int)instruction.op);
        NL;
        break;
    }
}

void dump_ir_analyse(IrGenContext *ctx, StreamFile stream, bool need_before_steps) {
    if(need_before_steps) stream.out = fopen(stream.path, "ab");
    FILE* out = stream.out;

    fprintf(out, "Representacao Intermediaria dos recursos: "); NL;

    for(size_t i = 0; i < ctx->instructions.count; ++i) {
        print_ir(out, ctx->instructions.items[i], 0);
    }

    fclose(out);
    out = NULL;
}

void dump_program_debug(const char* src, GenerateDebugOptions options, const char* filename, const char* out_path) {
    char sanitized[256];
    char path[512];

    if(strcmp(out_path, "./out.asm") == 0) {
        sanitize_path(filename, sanitized, sizeof(sanitized));
        snprintf(path, sizeof(path), "./debug/%s.%s", sanitized, prio_to_str(options.priority));
    } else {
        memcpy(path, out_path, strlen(out_path) + 1);
    }

    FILE* out = fopen(path, "wb");
    if(!out) {
        fprintf(stderr, "Erro ao abrir o arquivo de sair");
        exit(WRONG_PATH);
    }

    StreamFile stream = { out, path, filename };
    out = NULL;

    Arena arena;
    init_arena(&arena, MB);
    DiagContext ctx;
    diag_init(&ctx, &arena);
    LineMap map;
    linemap_init(&map);

    char* preprocessed = preprocess_source(src, NULL, "stdlib", filename, &arena, &map, register_source_cb, &ctx);

    diag_set_linemap(&ctx, filename, &map);

    bool stream_closed = false;

    if(options.before_process) {
        dump_before_preprocess_analyse(src, stream);
        stream_closed = true;
    }

    if(options.preprocess) {
        dump_preprocess_analyse(preprocessed, stream, stream_closed);
        stream_closed = true;
    }

    if(options.lex) {
        Lexer lexer = create_lexer(preprocessed, filename);
        dump_lexer_analyse(&lexer, stream, stream_closed);
        stream_closed = true;
    }

    Node* program = NULL;

    if(options.parse || options.check || options.ir) {
        Lexer parser_lexer = create_lexer(preprocessed, filename);
        Parser parser = create_parser(&parser_lexer, &ctx, &arena);
        program = parse_program(&parser);
    }

    if(options.parse) {
        dump_parser_analyse(program, stream, stream_closed);
        stream_closed = true;
    }

    if(options.check) {
        CheckContext checkctx = create_checkctx(&arena, &ctx);
        dump_check_analyse(&checkctx, program, stream, stream_closed);
        stream_closed = true;
    }

    if(has_error(&ctx)) {
        fclose(stream.out);
        stream.out = NULL;
        arena_free(&arena);
        diag_report_all(&ctx);
        exit(SEMANTICAL_ERROR);
    }

    if(options.ir) {
        IrGenContext ctx = create_irgen_context(&arena);
        irgen_program(&ctx, program);
        dump_ir_analyse(&ctx, stream, stream_closed);
        stream_closed = true;
    }

    if(!stream_closed) {
        fclose(stream.out);
    }

    arena_free(&arena);
}

static bool has_any_option(GenerateDebugOptions options) {
    return (options.preprocess || options.lex || options.check ||
            options.parse || options.ir || options.assembly);
}

int main(int argc, char* argv[]) {
    if(argc < 2) {
        fprintf(stderr, "Por favor coloque algum recurso apos o compilador\n");
        fprintf(stderr, "luegiu [FLAGS] <arquivo>\n");
        exit(OUT_OF_PATH);
    }

    GenerateDebugOptions options = shift(argc, argv);

    if(options.paths.count == 0) {
        fprintf(stderr, "Por favor coloque algum arquivo para o compilador\n");
        exit(OUT_OF_PATH);
    }

    char* out_path = options.paths.out_path;

    uint8_t entry_units = 0;
    for(uint16_t i = 0; i < options.paths.count; ++i) {
        char* filepath = options.paths.resolved[i];
        char* src = readfile(filepath);

        if(has_any_option(options)) {
            dump_program_debug(src, options, filepath, out_path);
            continue;
        }

        Arena arena;
        init_arena(&arena, KB);
        DiagContext ctx;
        diag_init(&ctx, &arena);
        LineMap map;
        linemap_init(&map);

        StreamFile stream;
        stream.filename = filepath;

        char sanitized[256];
        char path[512];
        sanitize_path(filepath, sanitized, sizeof(sanitized));
        snprintf(path, sizeof(path), "./debug/%s.%s", sanitized, "asm");

        stream.out = fopen(path, "wb");
        stream.path = filepath;

        char* preprocess = preprocess_source(src, NULL, "stdlib", filepath, &arena,
            &map, register_source_cb, &ctx);
        diag_register_source(&ctx, filepath, src);
        diag_set_linemap(&ctx, filepath, &map);
        diag_set_linemap(&ctx, filepath, &map);

        Lexer lexer = create_lexer(preprocess, filepath);
        Parser parser = create_parser(&lexer, &ctx, &arena);

        Node* program = parse_program(&parser);

        CheckContext checker = create_checkctx(&arena, &ctx);
        check_program(&checker, program);

        if(checker.entry_function != NULL) entry_units++;

        if(has_error(&ctx)) {
            diag_report_all(&ctx);
            arena_free(&arena);
            free(src);
            fclose(stream.out);
            return COMPILE_ERROR;
        }

        IrGenContext ir_gen = create_irgen_context(&arena);
        irgen_program(&ir_gen, program);

        CodegenContext codegen = create_codegen(stream.out, &arena, checker.entry_function, &checker);
        emit_program(&codegen, &ir_gen);

        free(src);
        arena_free(&arena);
        diag_unregister(&ctx);
        fclose(stream.out);
    }

    if(entry_units > 1){
        fprintf(stdout, "Muitos entry points para um programa");
        return COMPILE_ERROR;
    } else if(entry_units < 1) {
        fprintf(stdout, "O arquivo deve conter 1 entry point (link start)");
        return COMPILE_ERROR;
    }

    return COMPILE_OK;
}
