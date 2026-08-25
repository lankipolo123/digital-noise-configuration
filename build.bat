@echo off
REM Builds tx_lite.exe with mingw-w64 gcc. Install it via MSYS2
REM (https://www.msys2.org/) - after installing, run this from the
REM "MSYS2 MinGW x64" shell or a cmd with mingw-w64\bin on PATH.

windres src\app.rc -O coff -o src\app_res.o
if %ERRORLEVEL% NEQ 0 (
    echo Resource compile failed.
    exit /b 1
)

gcc -std=c99 -Wall -Wextra -Wpedantic -Werror -mwindows -Os -s ^
    -fno-ident -fno-asynchronous-unwind-tables ^
    -ffunction-sections -fdata-sections -Wl,--gc-sections ^
    -o tx_lite.exe src\main.c src\connection.c src\device.c src\protocol.c src\serial_port.c src\app_res.o ^
    -ladvapi32 -lgdi32 -luser32

if %ERRORLEVEL% NEQ 0 (
    echo Build failed.
    exit /b 1
)
echo Build OK: tx_lite.exe
