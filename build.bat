@echo off
REM Builds OpenLasso.exe (Windows 10/11 x64).
REM Uses MSVC or g++ if found; otherwise downloads a portable compiler automatically.
setlocal
cd /d "%~dp0"

where cl >nul 2>nul
if %errorlevel%==0 goto msvc
where g++ >nul 2>nul
if %errorlevel%==0 goto mingw
if exist "toolchain\bin_path.txt" goto local

echo No compiler found: trying to download a portable one...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0fetch_compiler.ps1"
if not exist "toolchain\bin_path.txt" (
  echo.
  echo Could not get a compiler. Read the messages above.
  pause
  exit /b 1
)

:local
set /p GCCBIN=<"toolchain\bin_path.txt"
set "PATH=%GCCBIN%;%PATH%"
goto mingw

:msvc
rc /nologo app.rc
cl /nologo /EHsc /O2 /MT /utf-8 /std:c++17 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   main.cpp process_supervisor.cpp features.cpp app.res /Fe:OpenLasso.exe ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:NO user32.lib gdi32.lib comctl32.lib advapi32.lib shell32.lib ^
   comdlg32.lib powrprof.lib ole32.lib psapi.lib winhttp.lib gdiplus.lib pdh.lib
goto end

:mingw
windres app.rc -O coff -o app_res.o
g++ -std=c++17 -O2 -municode -mwindows -static -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 ^
    main.cpp process_supervisor.cpp features.cpp app_res.o -o OpenLasso.exe ^
    -luser32 -lgdi32 -lcomctl32 -ladvapi32 -lshell32 -lcomdlg32 -lpowrprof -lole32 -lpsapi -lwinhttp -lgdiplus -lpdh

:end
if exist OpenLasso.exe (echo. & echo OK: OpenLasso.exe created.) else (echo Build failed. & pause)
endlocal
