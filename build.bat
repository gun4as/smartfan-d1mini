@echo off
setlocal

:: Nolasa versiju no ota_updater.cpp
for /f "tokens=3 delims= " %%a in ('findstr /C:"#define FW_VERSION" src\ota_updater.cpp') do set VER=%%~a

echo.
echo ========================================
echo   SmartFan D1 Mini Builder v%VER%
echo ========================================
echo.

set OUTDIR=firmware_builds
if not exist %OUTDIR% mkdir %OUTDIR%

:: Notira veco build
if exist .pio\build\d1_mini rmdir /s /q .pio\build\d1_mini

echo [1/2] Building firmware...
%USERPROFILE%\AppData\Roaming\Python\Python312\Scripts\pio.exe run -e d1_mini >nul 2>&1
if errorlevel 1 (
    echo FAILED: firmware
    exit /b 1
)
copy /y .pio\build\d1_mini\firmware.bin %OUTDIR%\d1_mini_%VER%.bin >nul
echo       OK - %OUTDIR%\d1_mini_%VER%.bin

echo [2/2] Building LittleFS...
%USERPROFILE%\AppData\Roaming\Python\Python312\Scripts\pio.exe run -e d1_mini -t buildfs >nul 2>&1
if errorlevel 1 (
    echo FAILED: littlefs
    exit /b 1
)
copy /y .pio\build\d1_mini\littlefs.bin %OUTDIR%\d1_mini_littlefs_%VER%.bin >nul
echo       OK - %OUTDIR%\d1_mini_littlefs_%VER%.bin

echo.
echo ========================================
echo   Gatavs! Faili %OUTDIR%\
echo ========================================
dir /b %OUTDIR%\*_%VER%.bin
echo.
