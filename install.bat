@echo off
setlocal EnableDelayedExpansion
title vdisk installer

echo ============================================================
echo   vdisk installer
echo ============================================================
echo.

rem --- Require winget -------------------------------------------------------
where winget >nul 2>&1
if errorlevel 1 (
    echo ERROR: 'winget' was not found.
    echo Install "App Installer" from the Microsoft Store, then run this again.
    echo.
    pause
    exit /b 1
)

rem --- WinFsp (required for everything) -------------------------------------
echo [1/3] Installing WinFsp ^(required^)...
winget install --id WinFsp.WinFsp -e --accept-source-agreements --accept-package-agreements
echo.

rem --- QEMU (only for 'vdisk linux -s', the real Linux VM) ------------------
echo [2/3] Installing QEMU ^(for 'vdisk linux -s'; optional but recommended^)...
winget install --id SoftwareFreedomConservancy.QEMU -e --accept-source-agreements --accept-package-agreements
echo.

rem --- Add this folder to the user PATH ------------------------------------
echo [3/3] Adding vdisk to your PATH...
set "DIR=%~dp0"
if "%DIR:~-1%"=="\" set "DIR=%DIR:~0,-1%"
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$d='%DIR%'; $p=[Environment]::GetEnvironmentVariable('PATH','User'); if([string]::IsNullOrEmpty($p)){$p=''}; if($p -notlike '*'+$d+'*'){[Environment]::SetEnvironmentVariable('PATH',($p.TrimEnd(';')+';'+$d).TrimStart(';'),'User'); Write-Host '[OK] Added to PATH.'} else {Write-Host '[OK] Already in PATH.'}"

echo.
echo ============================================================
echo   Done!  Open a NEW terminal (so PATH refreshes) and try:
echo.
echo     vdisk help
echo     vdisk create ram 512M R:
echo     vdisk linux -s R          (needs QEMU)
echo ============================================================
echo.
pause
