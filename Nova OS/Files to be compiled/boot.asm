; boot.asm
section .multiboot
align 4
    dd 0x1BADB002
    dd 0x03
    dd -(0x1BADB002 + 0x03)

section .text
[bits 32]
global _start
extern _kmain
global _keyboard_handler_wrapper
extern _keyboard_handler_c

_start:
    mov esp, 0x90000
    push eax
    push ebx
    call _kmain
    cli
    jmp $

_keyboard_handler_wrapper:
    pusha
    call _keyboard_handler_c
    popa
    iret