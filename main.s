section .data
    msg1 db "Para o teste final tente essa string completamente enorme e gigantesca", 10, 0
    msg2 db "Para outro teste supremamente supremo supremado, vamo com essa string", 10, 0

section .bss

section .text
    global start

start:
    lea rdi, [rel msg1]
    call .printf
    lea rdi, [rel msg2]
    call .printf
    jmp .exit

.printf:
    call .strlen
    mov rdx, rax
    mov rsi, rdi
    mov rax, 1
    mov rdi, 1
    syscall
    ret

.strlen:
    mov rax, rdi
    xor rcx, rcx
    jmp .loop

.loop:
    mov bl, [rax]
    cmp bl, 0
    je .strlen_exit
    add rcx, 1
    add rax, 1
    jmp .loop

.strlen_exit:
    mov rax, rcx
    ret

.exit:
    mov rax, 60
    xor rdi, rdi
    syscall
    
