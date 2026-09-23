; Nova OS Stage2 - 保护模式内核加载器
BITS 16
ORG 0x8000

stage2_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    ; 快速 A20
    in  al, 0x92
    or  al, 2
    out 0x92, al

    lgdt [gdt_desc]

    mov eax, cr0
    or  eax, 1
    mov cr0, eax

    jmp dword 0x08:pm_start

BITS 32
pm_start:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000

    ; 读引导信息扇区（LBA 33）到 0x70000
    mov edi, 0x70000
    mov ebx, 33
    call ata_read_one

    mov edi, 0x70000
    cmp dword [edi], 0x4E4F5641     ; "NOVA"
    jne boot_failed

    mov ebx, [edi + 4]              ; kernel_lba
    mov ecx, [edi + 8]              ; kernel_sectors
    mov edi, [edi + 12]              ; load_address
    mov esi, 0x70000
    mov edx, [esi + 20]              ; 正确读取 bss_start
    mov ebp, [esi + 24]              ; 正确读取 bss_size

    ; 加载内核扇区到 load_address
.load_loop:
    test ecx, ecx
    jz  .load_done
    push ecx
    push ebx
    push edi
    call ata_read_one
    pop edi
    pop ebx
    pop ecx
    add edi, 512
    inc ebx
    dec ecx
    jmp .load_loop

.load_done:
    ; 清零 BSS
    test ebp, ebp
    jz  .no_bss
    mov edi, edx
    mov ecx, ebp
    xor eax, eax
    rep stosb
.no_bss:
    ; 跳进内核
    mov eax, [0x70000 + 16]
    push 0
    push 0
    call eax

    hlt

boot_failed:
    hlt
    jmp boot_failed

; === 读一个扇区：EBX=LBA，EDI=目标 ===
ata_read_one:
    push eax
    push ecx
    push edx

    mov dx, 0x1F6
    mov eax, ebx
    shr eax, 24
    and al, 0x0F
    or  al, 0xE0
    out dx, al

    mov dx, 0x1F2
    mov al, 1
    out dx, al

    mov dx, 0x1F3
    mov al, bl
    out dx, al

    mov dx, 0x1F4
    mov eax, ebx
    shr eax, 8
    out dx, al

    mov dx, 0x1F5
    mov eax, ebx
    shr eax, 16
    out dx, al

    mov dx, 0x1F7
    mov al, 0x20
    out dx, al

.bsy:
    in  al, dx
    test al, 0x80
    jnz .bsy
.drq:
    in  al, dx
    test al, 0x08
    jz  .drq

    mov dx, 0x1F0
    mov ecx, 256
.words:
    in  ax, dx
    mov [edi], ax
    add edi, 2
    loop .words

    pop edx
    pop ecx
    pop eax
    ret

align 8
gdt_start:
    dq 0
    ; 代码段
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xCF, 0x00
    ; 数据段
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0xCF, 0x00
gdt_end:

gdt_desc:
    dw gdt_end - gdt_start - 1
    dd gdt_start

times 16*1024-($-$$) db 0