@echo off
setlocal

set ROOT=%~dp0
set OUT=%ROOT%out
set ISO=%ROOT%iso

echo ==========================================
echo  Nova OS ISO pack
echo ==========================================
echo ROOT = [%ROOT%]
echo OUT  = [%OUT%]
echo ISO  = [%ISO%]
echo.

if not exist "%OUT%\myos.elf" (
    echo ERROR: %OUT%\myos.elf not found.
    echo        Run MYOS.BAT first.
    pause
    exit /b 1
)

if not exist "%ISO%\boot\grub" mkdir "%ISO%\boot\grub"

echo [1/3] Copying myos.elf to iso\boot\...
copy /Y "%OUT%\myos.elf" "%ISO%\boot\myos.elf" >nul
if errorlevel 1 goto :err

echo [2/3] Writing grub.cfg...
(
    echo set timeout=3
    echo set default=0
    echo.
    echo menuentry "Nova OS" {
    echo     multiboot /boot/myos.elf
    echo     boot
    echo }
) > "%ISO%\boot\grub\grub.cfg"

if not exist "%ISO%\boot\grub\eltorito.img" (
    echo.
    echo ERROR: missing "%ISO%\boot\grub\eltorito.img"
    pause
    exit /b 1
)

echo [3/3] Packing nova.iso...
pushd "%ROOT%"
.\mkisofs.exe -R -allow-lowercase ^
    -b boot/grub/eltorito.img -no-emul-boot -boot-load-size 4 -boot-info-table ^
    -o nova.iso iso
if errorlevel 1 goto :err
popd

echo.
echo ==========================================
echo  ISO OK: %ROOT%nova.iso
echo ==========================================
pause
goto :eof

:err
echo.
echo !!! PACK FAILED (errorlevel=%errorlevel%) !!!
popd
pause
exit /b 1