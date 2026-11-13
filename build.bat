@echo off
rem Builds mariotennis-ds.nds with devkitPro. Output goes to build.log.
cd /d "%~dp0"
set CHERE_INVOKING=1
set MSYSTEM=MSYS
echo Building... > build.log
C:\devkitPro\msys2\usr\bin\bash.exe -lc "make %* >> build.log 2>&1; echo EXIT_CODE=$? >> build.log"
type build.log
