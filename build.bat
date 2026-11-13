@echo off
rem Builds mariotennis-ds.nds: finds your ROM, generates the game code,
rem then compiles with devkitPro. Full compiler output goes to build.log.
setlocal
cd /d "%~dp0"
echo Mario's Tennis DS build
echo.

set "PY="
py -3 --version >nul 2>&1 && set "PY=py -3"
if not defined PY python --version >nul 2>&1 && set "PY=python"
if not defined PY goto nopython

%PY% tools\generate.py
if errorlevel 1 goto failed

if not exist C:\devkitPro\msys2\usr\bin\bash.exe goto nodevkitpro
echo.
echo Compiling (a few minutes the first time)...
set CHERE_INVOKING=1
set MSYSTEM=MSYS
C:\devkitPro\msys2\usr\bin\bash.exe -lc "make %* > build.log 2>&1; echo EXIT_CODE=$? >> build.log"
findstr /c:"EXIT_CODE=0" build.log >nul
if errorlevel 1 goto compilefailed
echo.
echo Done: mariotennis-ds.nds
echo It contains your ROM, so do not share it.
goto end

:nopython
echo Python was not found.
echo Install it from https://www.python.org/downloads/
echo and tick "Add python.exe to PATH" in the installer, then run this again.
goto failed

:nodevkitpro
echo devkitPro was not found in C:\devkitPro.
echo Install it from https://devkitpro.org/wiki/Getting_Started
echo with "NDS Development" selected, then run this again.
goto failed

:compilefailed
type build.log

:failed
echo.
echo BUILD FAILED

:end
echo.
pause
