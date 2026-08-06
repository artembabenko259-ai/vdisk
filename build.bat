@echo off
setlocal

rem --- Locate the MSVC build environment (vcvars64.bat) ---------------------
set "VCVARS="
for /f "usebackq delims=" %%i in (`
    "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" ^
        -latest -prerelease -products * ^
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
        -find "VC\Auxiliary\Build\vcvars64.bat" 2^>nul
`) do set "VCVARS=%%i"

if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat"
)

if not defined VCVARS (
    echo ERROR: Could not find vcvars64.bat. Install the MSVC C/C++ build tools.
    exit /b 1
)

rem --- Locate the WinFsp SDK via the registry -------------------------------
set "WINFSP="
for /f "tokens=2,*" %%a in ('reg query "HKLM\SOFTWARE\WOW6432Node\WinFsp" /v InstallDir 2^>nul ^| findstr /i "InstallDir"') do set "WINFSP=%%b"

if not defined WINFSP (
    echo ERROR: WinFsp not found. Install it from https://winfsp.dev ^(with the Developer/SDK feature^).
    exit /b 1
)

call "%VCVARS%"

rem --- Compile --------------------------------------------------------------
rem winfsp-x64.dll is delay-loaded so the exe starts without it on PATH;
rem FspLoad() (called at startup) loads it from the registry InstallDir.
cl.exe /nologo /O2 /W3 ^
    /I "%WINFSP%inc" ^
    main.c disk_manager.c vram_allocator.c vdisk_util.c fs_memfs.c linux_shell.c block_disk.c qemu_linux.c save_disk.c accel.c bridge.c packages.c nfsclient.c fs_nfsclient.c ^
    "%WINFSP%lib\winfsp-x64.lib" urlmon.lib shell32.lib virtdisk.lib ws2_32.lib ^
    /Fe:vdisk.exe ^
    /link /DELAYLOAD:winfsp-x64.dll delayimp.lib advapi32.lib

if %ERRORLEVEL% EQU 0 (
    echo.
    echo BUILD SUCCESSFUL: vdisk.exe created!
) else (
    echo.
    echo BUILD FAILED!
    exit /b 1
)
