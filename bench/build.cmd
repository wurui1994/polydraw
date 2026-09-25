@echo off
rem ============================================================================
rem  build.cmd -- build the *original* PolyDraw (with the x87 JIT) plus the
rem               "/bench:N" instrumentation, using MSVC on Windows.
rem
rem  Run this from a "x86 Native Tools Command Prompt" (or call vcvars32.bat
rem  first).  x86 is NOT optional: kplib.c has nine 32-bit inline asm blocks and
rem  eval.c's whole JIT emits x87 machine code, so an x64 build cannot exist.
rem
rem  /FORCE:MULTIPLE is needed because `mysrand` is defined twice in the stock
rem  source (polydraw.c and eval.c).  Both definitions are identical; patching
rem  the source to remove one would make this build stop being "the original".
rem ============================================================================
setlocal
cd /d "%~dp0..\polydraw_src"

if "%1"=="clean" ( del /q *.obj polydraw.exe 2>nul & goto :eof )

cl /nologo /O2 /c polydraw.c eval.c kplib.c
if errorlevel 1 exit /b 1

link /nologo polydraw.obj eval.obj kplib.obj ^
     opengl32.lib glu32.lib gdi32.lib user32.lib comdlg32.lib winmm.lib ^
     /FORCE:MULTIPLE /OUT:polydraw.exe
if errorlevel 1 exit /b 1

echo built %CD%\polydraw.exe
