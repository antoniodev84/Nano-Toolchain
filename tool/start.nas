section .text
global _start
extern main
_start:
    mov rdi, [rsp]
    lea rsi, [rsp+8]
    and rsp, -16
    call main
    mov edi, eax
    mov eax, 60
    syscall
