# ============================================================
#  Nova OS v1.0 Makefile (Windows cmd.exe)
#  运行方式:  在 cmd 里输入 make
# ============================================================

SHELL       := cmd.exe
.SHELLFLAGS := /c

SRC_DIR   := src
OUT_DIR   := out
BUILD_DIR := build
ISO_DIR   := iso

NASM    := nasm
CC      := gcc
OBJCOPY := objcopy
MKISOFS := mkisofs.exe
QEMU    := qemu-system-i386
QEMUIMG := qemu-img

CFLAGS  := -m32 -ffreestanding -nostdlib -fno-pie -fno-pic -mno-stack-arg-probe -O2 -w
LDFLAGS := -m32 -nostdlib -Wl,--no-warn-rwx-segments

KERNEL   := $(OUT_DIR)\myos.elf
DISK_IMG := disk.img
ISO      := nova.iso

MBR_BIN  := $(BUILD_DIR)\mbr.bin
STG2_BIN := $(BUILD_DIR)\stage2.bin
MBR_OBJ  := $(BUILD_DIR)\mbr_bin.o
STG2_OBJ := $(BUILD_DIR)\stage2_bin.o
BOOT_OBJ := $(BUILD_DIR)\boot.o
KERN_OBJ := $(BUILD_DIR)\kernel.o
MYOS_EXE := $(BUILD_DIR)\myos.exe

.PHONY: all iso run run-disk clean distclean help

# ---------------- 默认 ----------------
all: $(KERNEL)
	@echo.
	@echo ============================================
	@echo   Build OK: $(KERNEL)
	@echo ============================================

# ---------------- 目录 ----------------
$(BUILD_DIR):
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"

$(OUT_DIR):
	@if not exist "$(OUT_DIR)" mkdir "$(OUT_DIR)"

# ---------------- 1/8 MBR ----------------
$(MBR_BIN): $(SRC_DIR)\mbr.asm | $(BUILD_DIR)
	@echo [1/8] Assembling MBR...
	$(NASM) -f bin "$(SRC_DIR)\mbr.asm" -o "$(MBR_BIN)"

# ---------------- 2/8 Stage2 ----------------
$(STG2_BIN): $(SRC_DIR)\stage2.asm | $(BUILD_DIR)
	@echo [2/8] Assembling Stage2...
	$(NASM) -f bin "$(SRC_DIR)\stage2.asm" -o "$(STG2_BIN)"

# ---------------- 3/8 bin -> obj ----------------
$(MBR_OBJ): $(MBR_BIN)
	@echo [3/8] mbr.bin -^> mbr_bin.o
	$(OBJCOPY) -I binary -O elf32-i386 -B i386 "$(MBR_BIN)" "$(MBR_OBJ)"

$(STG2_OBJ): $(STG2_BIN)
	@echo [4/8] stage2.bin -^> stage2_bin.o
	$(OBJCOPY) -I binary -O elf32-i386 -B i386 "$(STG2_BIN)" "$(STG2_OBJ)"

# ---------------- 5/8 boot.asm ----------------
$(BOOT_OBJ): $(SRC_DIR)\boot.asm | $(BUILD_DIR)
	@echo [5/8] Assembling boot.asm...
	$(NASM) -f elf32 "$(SRC_DIR)\boot.asm" -o "$(BOOT_OBJ)"

# ---------------- 6/8 kernel.c ----------------
$(KERN_OBJ): $(SRC_DIR)\kernel.c | $(BUILD_DIR)
	@echo [6/8] Compiling kernel.c...
	$(CC) $(CFLAGS) -c "$(SRC_DIR)\kernel.c" -o "$(KERN_OBJ)"

# ---------------- 7/8 Link ----------------
$(MYOS_EXE): $(BOOT_OBJ) $(KERN_OBJ) $(MBR_OBJ) $(STG2_OBJ) $(SRC_DIR)\linker.ld
	@echo [7/8] Linking...
	$(CC) $(LDFLAGS) -T "$(SRC_DIR)\linker.ld" -o "$(MYOS_EXE)" ^
	    "$(BOOT_OBJ)" "$(KERN_OBJ)" "$(MBR_OBJ)" "$(STG2_OBJ)"

# ---------------- 8/8 Final ELF ----------------
$(KERNEL): $(MYOS_EXE) | $(OUT_DIR)
	@echo [8/8] Generating final ELF...
	$(OBJCOPY) -O elf32-i386 "$(MYOS_EXE)" "$(KERNEL)"

# ---------------- ISO ----------------
iso: $(KERNEL)
	@echo Packing ISO...
	@if not exist "$(ISO_DIR)\boot\grub" mkdir "$(ISO_DIR)\boot\grub"
	@copy /Y "$(KERNEL)" "$(ISO_DIR)\boot\myos.elf" >nul
	@(echo set timeout=3& echo set default=0& echo.& echo menuentry "Nova OS" {& echo     multiboot /boot/myos.elf& echo     boot& echo }) > "$(ISO_DIR)\boot\grub\grub.cfg"
	@if not exist "$(ISO_DIR)\boot\grub\eltorito.img" (echo ERROR: missing eltorito.img & exit /b 1)
	$(MKISOFS) -R -allow-lowercase -b boot/grub/eltorito.img -no-emul-boot -boot-load-size 4 -boot-info-table -o $(ISO) "$(ISO_DIR)"
	@echo ==^> ISO ready: $(ISO)

# ---------------- QEMU ----------------
$(DISK_IMG):
	$(QEMUIMG) create $(DISK_IMG) 256M

run: $(KERNEL) iso $(DISK_IMG)
	$(QEMU) -cdrom $(ISO) -hda $(DISK_IMG) -m 256M -boot d

run-disk: $(DISK_IMG)
	$(QEMU) -hda $(DISK_IMG) -m 256M

# ---------------- 清理 ----------------
clean:
	@if exist "$(BUILD_DIR)" rd /s /q "$(BUILD_DIR)"
	@if exist "$(KERNEL)"      del /q "$(KERNEL)"
	@if exist "$(SRC_DIR)\mbr.bin"      del /q "$(SRC_DIR)\mbr.bin"
	@if exist "$(SRC_DIR)\stage2.bin"   del /q "$(SRC_DIR)\stage2.bin"
	@if exist "$(SRC_DIR)\mbr_bin.o"    del /q "$(SRC_DIR)\mbr_bin.o"
	@if exist "$(SRC_DIR)\stage2_bin.o" del /q "$(SRC_DIR)\stage2_bin.o"
	@if exist "$(SRC_DIR)\boot.o"       del /q "$(SRC_DIR)\boot.o"
	@if exist "$(SRC_DIR)\kernel.o"     del /q "$(SRC_DIR)\kernel.o"
	@if exist "$(SRC_DIR)\myos.exe"     del /q "$(SRC_DIR)\myos.exe"
	@if exist "$(SRC_DIR)\myos.elf"     del /q "$(SRC_DIR)\myos.elf"
	@echo clean done.

distclean: clean
	@if exist $(ISO)      del /q $(ISO)
	@if exist $(DISK_IMG) del /q $(DISK_IMG)
	@if exist "$(ISO_DIR)\boot\myos.elf"          del /q "$(ISO_DIR)\boot\myos.elf"
	@if exist "$(ISO_DIR)\boot\grub\grub.cfg"     del /q "$(ISO_DIR)\boot\grub\grub.cfg"
	@echo distclean done.

help:
	@echo Nova OS build targets:
	@echo   make          - build kernel
	@echo   make iso      - build + pack nova.iso
	@echo   make run      - boot from CD-ROM with empty disk
	@echo   make run-disk - boot from disk only
	@echo   make clean    - remove build products
	@echo   make distclean - remove everything