@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat"
cl.exe /nologo /O2 /W3 main.c vram_allocator.c vram_proxy.c imdisk_driver.c vhd_driver.c disk_manager.c advapi32.lib user32.lib shell32.lib virtdisk.lib /Fe:vdisk.exe
if %ERRORLEVEL% EQU 0 (
    echo.
    echo BUILD SUCCESSFUL: vdisk.exe created!
) else (
    echo.
    echo BUILD FAILED!
)
