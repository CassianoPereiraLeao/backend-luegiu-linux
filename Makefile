CC = gcc
CFLAGS = -g -Wall -Wextra -Wimplicit-fallthrough

NASM = nasm
NASMFLAGS = -f elf64

LD = gcc
LDFLAGS = -nostartfiles -e start

COMPILADOR = ./out/luegiu
FONTE = ./uses/main.luegiu
ASM = ./out/out.asm
OBJ = ./out/output.o
PROGRAMA_FLAGS = -Preprocess -Lex -Parse -Check -Ir -Codegen
PROGRAMA = ./out/program

SOURCES = $(wildcard *.c arena/*.c diagnostics/*.c lex/*.c parser/*.c ir/*.c codegen/linux/*.c preprocess/*.c)

all: build run assemble link execute exec_luegiu link_luegiu run_luegiu

build:
	$(CC) $(CFLAGS) $(SOURCES) -o $(COMPILADOR)

run:
	$(COMPILADOR) ${PROGRAMA_FLAGS} $(FONTE) ${ASM}

assemble:
	$(NASM) $(NASMFLAGS) $(ASM) -o $(OBJ)

link:
	$(LD) $(LDFLAGS) $(OBJ) -o $(PROGRAMA)

execute:
	$(PROGRAMA)

exec_luegiu:
	${NASM} ${NASMFLAGS} ./debug/out.s -o ./debug/luegiu.o
	
link_luegiu:
	${LD} -nostartfiles -e _start ./debug/luegiu.o -o ./compiler/luegiu

run_luegiu:
	./compiler/luegiu

clean:
	rm -f $(COMPILADOR) $(OBJ) $(ASM) $(PROGRAMA)