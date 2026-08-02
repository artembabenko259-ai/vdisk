@echo off
title Add vdisk to Windows PATH

echo Registering vdisk in User PATH...

set "TARGET_DIR=%~dp0"
if "%TARGET_DIR:~-1%"=="\" set "TARGET_DIR=%TARGET_DIR:~0,-1%"

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$dir = '%TARGET_DIR%';" ^
    "$currentPath = [Environment]::GetEnvironmentVariable('PATH', 'User');" ^
    "if ($currentPath -notlike '*' + $dir + '*') {" ^
    "    [Environment]::SetEnvironmentVariable('PATH', $currentPath + ';' + $dir, 'User');" ^
    "    Write-Host '[SUCCESS] Successfully added ' $dir ' to User PATH!' -ForegroundColor Green;" ^
    "} else {" ^
    "    Write-Host '[INFO] ' $dir ' is already in User PATH.' -ForegroundColor Yellow;" ^
    "}"

echo.
echo You can now use the 'vdisk' command from any terminal or command prompt!
