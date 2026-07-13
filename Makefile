TARGET = main.exe

ASM_SRC = main.s
OBJ = main.obj

AS = nasm
LD = gcc

ASM_FLAGS = -f win64
LD_FLAGS = -nostdlib -e start

all: $(TARGET) run

$(TARGET): $(OBJ)
	$(LD) $(LD_FLAGS) $(OBJ) -o $(TARGET)

$(OBJ): $(ASM_SRC)
	$(AS) $(ASM_FLAGS) $(ASM_SRC) -o $(OBJ)

run:
	./main.exe

clean:
	del $(OBJ) $(TARGET)