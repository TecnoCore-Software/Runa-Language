#!/usr/bin/env bash
# Runa toolchain build script (GCC, direct compile — no CMake yet).
# Windows/MinGW users: use build.bat instead.
set -euo pipefail
cd "$(dirname "$0")"

CXX="${CXX:-g++}"
CXXFLAGS="-std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc"

CORE_SOURCES="
src/errors/errors.cpp
src/lexer/lexer.cpp
src/lexer/unicode_tables.cpp
src/ast/ast.cpp
src/parser/parser.cpp
src/bytecode/bytecode.cpp
src/compiler/compiler.cpp
src/runtime/runtime.cpp
src/runtime/heap.cpp
src/runtime/native/native_loader.cpp
src/runtime/native/ffi_bridge.cpp
src/import/json.cpp
src/import/rpr.cpp
src/import/manifest.cpp
src/import/importer.cpp
src/import/platform.cpp
src/stdlib/stdlib.cpp
src/vm/vm.cpp
"

mkdir -p build

echo "[runa] building runac..."
$CXX $CXXFLAGS $CORE_SOURCES src/tools/runac_main.cpp -o build/runac

echo "[runa] building runavm..."
$CXX $CXXFLAGS $CORE_SOURCES src/tools/runavm_main.cpp -o build/runavm

echo "[runa] building runa (combined)..."
$CXX $CXXFLAGS $CORE_SOURCES src/main.cpp -o build/runa

echo "[runa] building rpr (package tool)..."
$CXX $CXXFLAGS src/import/json.cpp src/import/rpr.cpp src/import/manifest.cpp \
    src/import/importer.cpp src/tools/rpr_main.cpp -o build/rpr

echo "[runa] building nruna (native tool)..."
$CXX $CXXFLAGS src/import/json.cpp src/import/rpr.cpp src/import/platform.cpp \
    src/import/manifest.cpp src/runtime/native/native_loader.cpp \
    src/tools/nruna_main.cpp -o build/nruna

echo "[runa] build OK -> build/runac build/runavm build/runa build/rpr build/nruna"
