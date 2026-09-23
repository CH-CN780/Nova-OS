@echo off
setlocal

set ROOT=%~dp0
set SRC=%ROOT%src
set OUT=%ROOT%out

echo ==========================================
echo  Nova OS v1.0 build
echo ==========================================
echo ROOT = [%ROOT%]
echo SRC  = [%SRC%]
echo OUT  = [%OUT%]
echo.

if not exist "%SRC%" (
    echo ERROR: source directory not found:
    echo        %SRC%
    pause
    exit /b 1
)

if not exist "%OUT%" mkdir "%OUT%"

pushd "%SRC%"
if errorlevel 1 (
    echo ERROR: cannot enter source directory
    pause
    exit /b 1
)
del /q *.o *.exe *.elf *.bin
echo [1/8] Assembling MBR...
nasm -f bin mbr.asm -o mbr.bin
if errorlevel 1 goto :err

echo [2/8] Assembling Stage2...
nasm -f bin stage2.asm -o stage2.bin
if errorlevel 1 goto :err

echo [3/8] MBR -^> ELF object...
objcopy -I binary -O elf32-i386 -B i386 mbr.bin mbr_bin.o
if errorlevel 1 goto :err

echo [4/8] Stage2 -^> ELF object...
objcopy -I binary -O elf32-i386 -B i386 stage2.bin stage2_bin.o
if errorlevel 1 goto :err

echo [5/8] Assembling boot.asm...
nasm -f elf32 boot.asm -o boot.o
if errorlevel 1 goto :err

echo [6/8] Compiling kernel.c...
gcc -m32 -ffreestanding -nostdlib -fno-pie -fno-pic -fno-leading-underscore -mno-stack-arg-probe -mgeneral-regs-only -O2 -w -c kernel.c -o kernel.o
if errorlevel 1 goto :err

echo [7/8] Linking (static, no reloc section)...
gcc -m32 -nostdlib -T linker.ld -o myos.exe ^
    boot.o kernel.o mbr_bin.o stage2_bin.o ^
    -Wl,--no-warn-rwx-segments ^
    -Wl,--disable-reloc-section ^
    -Wl,--disable-dynamicbase ^
    -Wl,--disable-nxcompat ^
    -static -no-pie
if errorlevel 1 goto :err

echo [8/8] Final ELF...
objcopy -O elf32-i386 myos.exe myos.elf
if errorlevel 1 goto :err

copy /Y myos.elf "%OUT%\myos.elf" >nul

popd

echo.
echo ==========================================
echo  Build OK: %OUT%\myos.elf
echo ==========================================
pause
goto :eof

:err
echo.
echo !!! BUILD FAILED (errorlevel=%errorlevel%) !!!
popd
pause
exit /b 1