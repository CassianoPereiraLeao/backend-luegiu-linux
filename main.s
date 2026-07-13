section .data
    msg db "Hello NASM!", 10, 0
    len equ $ - msg

section .bss

section .text
    global start

start:
    mov r8, 0
    jmp .loop

.print:
    mov rax, 1
    mov rdi, 1
    lea rsi, [rel msg]
    lea rdx, len
    syscall
    ret

.loop:
    cmp r8, 10
    je .exit
    add r8, 1
    call .print
    jmp .loop

.exit:
    mov rax, 60
    xor rdi, rdi
    syscall
    
