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

rem 编的是 *.stitch.c（`omni c split` 生成的缝合文件，ADR-0046）：它只有一串
rem `#include`，把拆开的那几十份按**原次序**接回来 —— 编译器看见的记号流与原文一模一样，
rem 原始代码一个字节都没改。要退回原文就把这三个名字去掉 `.stitch`。
rem 为什么不各编成 .o：那些 `static` 只在原来那一个翻译单元里可见，分开编要去掉 `static`
rem 并补 `extern`，那是改原文。
cl /nologo /O2 /c polydraw.stitch.c eval.stitch.c kplib.stitch.c
if errorlevel 1 exit /b 1

link /nologo polydraw.stitch.obj eval.stitch.obj kplib.stitch.obj ^
     opengl32.lib glu32.lib gdi32.lib user32.lib comdlg32.lib winmm.lib ^
     /FORCE:MULTIPLE /OUT:polydraw.exe
if errorlevel 1 exit /b 1

echo built %CD%\polydraw.exe
