@echo off
REM ==========================================================================
REM build.bat [UnityEditorDir(optional)]
REM   1. compile gen_exports.exe (build-time tool, zero dependencies)
REM   2. run generator: parse local System32\d3d12.dll export table,
REM      emit generated_pragmas.h / generated_thunks.inc / generated_names.h
REM      if editor dir is given, also verify Unity.dll d3d12 imports coverage
REM   3. assemble + compile + link the d3d12.dll proxy
REM Rerun after any Windows update to re-sync ordinals/exports.
REM ==========================================================================
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 (echo vcvars64 failed & exit /b 1)
cd /d %~dp0

cl /nologo /std:c++17 /MT /O2 /W3 /utf-8 gen_exports.cpp /Fe:gen_exports.exe || exit /b 1
gen_exports.exe "%~1" || exit /b 1

ml64 /nologo /c thunks.asm /Fo:thunks.obj || exit /b 1
cl /nologo /c /std:c++17 /MT /O2 /W3 /utf-8 d3d12_proxy.cpp /Fo:d3d12_proxy.obj || exit /b 1
link /nologo /DLL /OUT:d3d12.dll d3d12_proxy.obj thunks.obj || exit /b 1
echo BUILD OK: %~dp0d3d12.dll
