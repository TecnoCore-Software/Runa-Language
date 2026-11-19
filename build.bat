@echo off
REM Runa toolchain build (GCC/MinGW, Windows).
REM Requires g++ in PATH (e.g. from MinGW-w64 / MSYS2).
setlocal
cd /d "%~dp0"

set CXX=g++
REM -static makes the exes self-contained (no MinGW DLLs needed at runtime).
REM If your MinGW lacks the static libs and the build fails, remove the
REM three -static... flags below and make sure MinGW's bin dir is in PATH.
set CXXFLAGS=-std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc -static -static-libgcc -static-libstdc++
set CORE=src\errors\errors.cpp src\lexer\lexer.cpp src\lexer\unicode_tables.cpp src\ast\ast.cpp src\parser\parser.cpp src\bytecode\bytecode.cpp src\compiler\compiler.cpp src\runtime\runtime.cpp src\runtime\heap.cpp src\runtime\native\native_loader.cpp src\runtime\native\ffi_bridge.cpp src\import\json.cpp src\import\rpr.cpp src\import\manifest.cpp src\import\importer.cpp src\import\platform.cpp src\stdlib\stdlib.cpp src\vm\vm.cpp
set IMPORTLIB=src\import\json.cpp src\import\rpr.cpp src\import\manifest.cpp src\import\importer.cpp

if not exist build mkdir build

echo [runa] building runac...
%CXX% %CXXFLAGS% %CORE% src\tools\runac_main.cpp -o build\runac.exe || goto :fail

echo [runa] building runavm...
%CXX% %CXXFLAGS% %CORE% src\tools\runavm_main.cpp -o build\runavm.exe || goto :fail

echo [runa] building runa (combined)...
%CXX% %CXXFLAGS% %CORE% src\main.cpp -o build\runa.exe || goto :fail

echo [runa] building rpr (package tool)...
%CXX% %CXXFLAGS% %IMPORTLIB% src\tools\rpr_main.cpp -o build\rpr.exe || goto :fail

echo [runa] building nruna (native tool)...
%CXX% %CXXFLAGS% src\import\json.cpp src\import\rpr.cpp src\import\platform.cpp src\import\manifest.cpp src\runtime\native\native_loader.cpp src\tools\nruna_main.cpp -o build\nruna.exe || goto :fail

echo [runa] build OK -^> build\runac.exe build\runavm.exe build\runa.exe build\rpr.exe build\nruna.exe
exit /b 0

:fail
echo [runa] build FAILED.
exit /b 1
