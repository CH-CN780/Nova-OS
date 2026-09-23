; ============================================================
;  boot.asm - Nova OS 内核入口 + 中断/异常包装器
; ============================================================
BITS 32

section .multiboot
align 4
    dd 0x1BADB002
    dd 0x00000003
    dd -(0x1BADB002 + 0x00000003)

section .text

global _start
extern kmain

global keyboard_handler_wrapper
extern keyboard_handler_c

global mouse_handler_wrapper
extern mouse_handler_c

global exception_handler_wrapper
extern exception_handler_c

; ============================================================
;  内核入口
; ============================================================
_start:
    cli
    mov esp, 0x90000        ; 用之前的栈位置

    ; --- 可视测试点：往 VGA 左上角写 "OK!" ---
    mov edi, 0xB8000
    mov word [edi],   0x0F4F     ; 'O' 白字
    mov word [edi+2], 0x0F4B     ; 'K'
    mov word [edi+4], 0x0F21     ; '!'

    push ebx
    push eax
    call kmain

.hang:
    cli
    hlt
    jmp .hang

; ============================================================
;  键盘中断包装器（IRQ1，IDT[0x21]）
; ============================================================
keyboard_handler_wrapper:
    pusha
    call keyboard_handler_c
    popa
    iretd

; ============================================================
;  鼠标中断包装器（IRQ12，IDT[0x2C]）
; ============================================================
mouse_handler_wrapper:
    pusha
    call mouse_handler_c
    popa
    iretd

; ============================================================
;  通用异常包装器（IDT 所有未处理槽位）
; ============================================================
exception_handler_wrapper:
    pusha
    call exception_handler_c
    popa
    iretd