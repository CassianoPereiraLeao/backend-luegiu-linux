CC = gcc
CFLAGS = -g -Wall -Wextra -Wimplicit-fallthrough

NASM = nasm
NASMFLAGS = -f elf64

LD = ld

COMPILADOR = ./out/luegiu
FONTES_LUEGIU = $(wildcard ./uses/*.luegiu)
PROGRAMA_FLAGS = -Preprocess -Lex -Parse -Check -Ir -Codegen
PROGRAMA = ./out/program

SOURCES = $(wildcard *.c arena/*.c diagnostics/*.c lex/*.c parser/*.c ir/*.c codegen/linux/*.c preprocess/*.c)

# Nomes dos .s esperados no ./debug, um por .luegiu, com '/' trocado por '_'
ASMS = $(patsubst ./uses/%.luegiu,./debug/._uses_%.luegiu.s,$(FONTES_LUEGIU))
OBJS = $(ASMS:.s=.o)

all: build run assemble link execute

build:
	$(CC) $(CFLAGS) $(SOURCES) -o $(COMPILADOR)

run:
	$(COMPILADOR) $(PROGRAMA_FLAGS) $(FONTES_LUEGIU)

assemble: $(OBJS)

%.o: %.s
	$(NASM) $(NASMFLAGS) $< -o $@

link: assemble
	$(LD) -o $(PROGRAMA) $(OBJS)

execute:
	$(PROGRAMA)

clean:
	rm -f $(COMPILADOR) $(PROGRAMA) ./debug/*.o ./debug/*.s

.PHONY: all build run assemble link execute clean