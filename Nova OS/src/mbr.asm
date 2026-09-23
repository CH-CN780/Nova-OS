; Nova OS MBR - 加载 stage2 到 0x8000 并跳转
BITS 16
ORG 0x7C00

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov [boot_drive], dl

    ; EDD 扩展读：从 LBA 1 读 32 扇区到 0x8000
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc  .fail

    jmp 0x0000:0x8000

.fail:
    mov si, fail_msg
.loop:
    lodsb
    test al, al
    jz  .halt
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp .loop
.halt:
    hlt
    jmp .halt

boot_drive: db 0
fail_msg:   db "Nova boot error", 0

align 4
dap:
    db 0x10, 0
    dw 32
    dw 0x8000
    dw 0x0000
    dq 1

times 510-($-$$) db 0
dw 0xAA55