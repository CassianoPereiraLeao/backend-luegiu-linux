CC = gcc
CFLAGS = -g

NASM = nasm
NASMFLAGS = -f elf64

LD = gcc
LDFLAGS = -nostartfiles -e start

COMPILADOR = ./out/luegiu
FONTE = ./uses/main.luegiu
ASM = ./out/out.asm
OBJ = ./out/output.o
PROGRAMA = ./out/programa

SOURCES = $(wildcard *.c arena/*.c diagnostics/*.c lex/*.c)

all: build run assemble link execute

build:
	$(CC) $(CFLAGS) $(SOURCES) -o $(COMPILADOR)

run:
	$(COMPILADOR) $(FONTE) $(ASM)

assemble:
	$(NASM) $(NASMFLAGS) $(ASM) -o $(OBJ)

link:
	$(LD) $(LDFLAGS) $(OBJ) -o $(PROGRAMA)

execute:
	$(PROGRAMA)

clean:
	rm -f $(COMPILADOR) $(OBJ) $(ASM) $(PROGRAMA)