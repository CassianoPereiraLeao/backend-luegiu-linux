CC = gcc
TARGET = bin\luegiu.exe

SRC_DIR = src
BUILD_DIR = build

WARNINGS = -Wall -Wextra -Wpedantic

CFLAGS = \
    $(WARNINGS) \
    -MMD -MP

DEBUG_FLAGS = -g -O0
RELEASE_FLAGS = -O3 -DNDEBUG

SRC = \
    $(wildcard src/*.c) \
	$(wildcard src/arena/*.c) \
    $(wildcard src/lexer/*.c) \
    $(wildcard src/parser/*.c) \
    $(wildcard src/helpers/*.c) \
    $(wildcard src/ir/*.c) \
    $(wildcard src/codegen/*.c) \
	$(wildcard src/diagnostic/*.c) \
	$(wildcard src/preprocess/*.c)

OBJ = $(SRC:.c=.o)
OBJ := $(subst src,build,$(OBJ))

DEP = $(OBJ:.o=.d)

all: debug

debug: CFLAGS += $(DEBUG_FLAGS)
debug: $(TARGET)

release: CFLAGS += $(RELEASE_FLAGS)
release: $(TARGET)

$(TARGET): $(OBJ)
	@if not exist bin mkdir bin
	$(CC) $(OBJ) -o $@

build/%.o: src/%.c
	@if not exist build mkdir build
	@if not exist build\lexer mkdir build\lexer
	@if not exist build\arena mkdir build\arena
	@if not exist build\parser mkdir build\parser
	@if not exist build\helpers mkdir build\helpers
	@if not exist build\ir mkdir build\ir
	@if not exist build\diagnostic mkdir build\diagnostic
	@if not exist build\preprocess mkdir build\preprocess
	@if not exist build\codegen mkdir build\codegen

	$(CC) $(CFLAGS) -c $< -o $@

run: debug
	$(TARGET)

clean:
	@if exist build rmdir /S /Q build
	@if exist bin rmdir /S /Q bin

rebuild: clean all

test-preprocess-source: all
	.\bin\luegiu.exe -preprocess .\tests\teste_preprocess.luegiu -out .\debug\preprocess.luegiu.preprocess

test-lexer-keywords: all
	.\bin\luegiu.exe -lex .\tests\teste_keywords.luegiu -out .\debug\verify_keywords.luegiu.lex

test-lexer-operators: all
	.\bin\luegiu.exe -lex .\tests\teste_operators.luegiu -out .\debug\verify_operators.luegiu.lex

test-parse-only-program: all
	.\bin\luegiu.exe -parse .\tests\teste_parser.luegiu

test-check-only-program: all
	.\bin\luegiu.exe -check .\tests\teste_typecheck.luegiu

test-ir-only-program: all
	.\bin\luegiu.exe -ir .\tests\teste_ir.luegiu

test-all-tags-program: all
	.\bin\luegiu.exe -bp -pp -lex -parse -check -ir .\tests\teste_all.luegiu

test-no-debug: all
	.\bin\luegiu.exe .\tests\teste_all.luegiu -out .\debug\out.asm

bootstrap: all
	.\bin\luegiu.exe .\bootstrap\src\main.luegiu .\bootstrap\lib\mmemory.luegiu -out .\debug\luegiu.asm

test-print: all
	.\bin\luegiu.exe -bp -preprocess .\bootstrap\src\main.luegiu .\bootstrap\src\lexer\lexer.luegiu

lib-test: all
	.\bin\luegiu.exe .\bootstrap\src\main.luegiu .\bootstrap\lib\mmemory.luegiu

-include $(DEP)

.PHONY: all debug release run clean rebuild