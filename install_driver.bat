@echo off
title ImDisk Driver Direct Installer

echo Installing ImDisk driver files to System32...

copy /y "%~dp0sys\amd64\imdisk.sys" "%SystemRoot%\System32\drivers\imdisk.sys"
copy /y "%~dp0cli\amd64\imdisk.exe" "%SystemRoot%\System32\imdisk.exe"
copy /y "%~dp0svc\amd64\imdsksvc.exe" "%SystemRoot%\System32\imdsksvc.exe"
copy /y "%~dp0cpl\amd64\imdisk.cpl" "%SystemRoot%\System32\imdisk.cpl"
copy /y "%~dp0imdisk.inf" "%SystemRoot%\System32\imdisk.inf"

echo Registering ImDisk services...
sc create ImDisk binPath= "%SystemRoot%\System32\drivers\imdisk.sys" type= kernel start= auto
sc create ImDskSvc binPath= "%SystemRoot%\System32\imdsksvc.exe" type= own start= auto

echo Starting services...
net start imdsksvc
net start imdisk

echo.
echo Done! ImDisk driver is fully installed.
