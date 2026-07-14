CC = gcc
CFLAGS = -g
NASM = nasm
NASM_FLAGS = -f win64
LINKER = gcc
LINKER_FLAGS = -nostartfiles -e start

COMPILADOR = ./out/luegiu.exe
FONTE_LUEGIU = ./uses/main.luegiu
ASSEMBLY = ./out/out.asm
OBJETO = ./out/output.obj
EXECUTAVEL_FINAL = ./out/programa.exe
SOURCES = ./*.c ./arena/*.c ./diagnostics/*.c ./lex/*.c

all: build run assemble link execute

build:
	$(CC) $(CFLAGS) ${SOURCES} -o $(COMPILADOR)

run:
	$(COMPILADOR) $(FONTE_LUEGIU) $(ASSEMBLY)

assemble:
	$(NASM) $(NASM_FLAGS) $(ASSEMBLY) -o $(OBJETO)

link:
	$(LINKER) $(LINKER_FLAGS) $(OBJETO) -o $(EXECUTAVEL_FINAL)

execute:
	$(EXECUTAVEL_FINAL) ${FONTE_LUEGIU}

clean_temp:
	@del -f $(OBJETO) $(ASSEMBLY)

clean:
	@del -f $(COMPILADOR) $(OBJETO) $(ASSEMBLY) $(EXECUTAVEL_FINAL)

parse:
	${CC} -g ./*.c -o ./out.exe

lex:
	${CC} -g ./*.c -o ./out.exe

test-parse:
	./out.exe -parse ${FONTE_LUEGIU}

test-lex:
	./out.exe -lex ${FONTE_LUEGIU}
