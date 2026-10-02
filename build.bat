@echo off
rem Build sound_ogg_hook.dll with MinGW-w64 and place it into ..\sound_ogg_hook_dll\.
rem The host-side check tools (verify_resolver.exe, selftest.exe) land in build\.
rem
rem Set MINGW_BIN if g++ is not on PATH, e.g.
rem   set MINGW_BIN=D:\Program Files\mingw64\bin
setlocal

if not "%MINGW_BIN%"=="" set "PATH=%MINGW_BIN%;%PATH%"

where g++ >nul 2>nul
if errorlevel 1 (
  echo g++ not found. Install MinGW-w64 and add its bin directory to PATH,
  echo or set MINGW_BIN to it.
  exit /b 1
)

set ROOT=%~dp0
set SRC=%ROOT%src
set TOOLS=%ROOT%tools
set BUILD=%ROOT%build
set OUT=%ROOT%..\sound_ogg_hook_dll

if not exist "%BUILD%" mkdir "%BUILD%"
if not exist "%OUT%" mkdir "%OUT%"

set CFLAGS=-O2 -DNDEBUG -fno-strict-aliasing
set COMMON=-std=c++17 -O2 -DNDEBUG -Wall -Wextra -static -static-libgcc -static-libstdc++

echo [1/5] stb_vorbis.c
gcc %CFLAGS% -c "%SRC%\stb_vorbis.c" -o "%BUILD%\stb_vorbis.o"
if errorlevel 1 exit /b 1

echo [2/5] sound_ogg_hook.dll
g++ %COMMON% -shared -o "%OUT%\sound_ogg_hook.dll" ^
  "%SRC%\dllmain.cpp" "%SRC%\hook.cpp" "%SRC%\resolver.cpp" "%SRC%\image.cpp" ^
  "%SRC%\patch.cpp" "%SRC%\rwops.cpp" "%SRC%\stream.cpp" "%SRC%\audio.cpp" "%SRC%\cache.cpp" ^
  "%SRC%\config.cpp" "%SRC%\prefetch.cpp" "%SRC%\sound_asset.cpp" "%SRC%\log.cpp" ^
  "%BUILD%\stb_vorbis.o" ^
  -lkernel32 -lshell32 -ladvapi32
if errorlevel 1 exit /b 1

echo [3/5] verify_resolver.exe
g++ %COMMON% -municode -I"%SRC%" -o "%BUILD%\verify_resolver.exe" "%TOOLS%\verify_resolver.cpp" ^
  "%SRC%\resolver.cpp" "%SRC%\image.cpp" "%SRC%\log.cpp" -lkernel32
if errorlevel 1 exit /b 1

echo [4/5] selftest.exe
g++ %COMMON% -municode -I"%SRC%" -o "%BUILD%\selftest.exe" "%TOOLS%\selftest.cpp" ^
  "%SRC%\audio.cpp" "%SRC%\rwops.cpp" "%SRC%\stream.cpp" "%SRC%\cache.cpp" "%SRC%\config.cpp" ^
  "%SRC%\sound_asset.cpp" "%SRC%\log.cpp" "%SRC%\image.cpp" "%BUILD%\stb_vorbis.o" -lkernel32
if errorlevel 1 exit /b 1

rem Runs the checks and also parses the sound_ogg_hook.ini we ship, so a typo in
rem a key name there fails the build instead of the game.
echo [5/5] selftest run
"%BUILD%\selftest.exe" "" "%OUT%"
if errorlevel 1 exit /b 1

echo.
echo done:
dir /b "%OUT%"
dir /b "%BUILD%"
endlocal
