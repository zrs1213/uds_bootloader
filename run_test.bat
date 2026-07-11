@echo off
set "PATH=C:\Windows\System32;C:\Windows;C:\Windows\System32\Wbem;C:\mingw64\bin;C:\mingw64\libexec\gcc\x86_64-w64-mingw32\16.1.0"
cd /d "C:\Users\zhangruosen\WorkBuddy\2026-07-06-11-18-16\uds_bootloader"
echo ============================================
echo   UDS Bootloader Test
echo ============================================
call C:\mingw64\bin\make.exe clean >nul 2>&1
call C:\mingw64\bin\make.exe test
echo.
echo 按任意键退出...
pause >nul
