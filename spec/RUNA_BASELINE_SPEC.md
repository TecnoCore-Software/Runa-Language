# Runa Language — Design Baseline (v0.1, unofficial)

> Status: INTRODUCTION ONLY. The language designer has NOT finalized the syntax yet.
> Nothing below may be implemented or extended without the designer's explicit approval.
> This document was provided by the language designer as the architectural baseline.

---

## 1. Language Identity

- **Name:** Runa
- **Source extension:** `.ru` (e.g. `main.ru`)
- **Status:** Independent language. NOT a Tluxe/TOSP language unless explicitly stated.
- **Implementation language (initial toolchain):** C++ (modern, understandable C++; RAII, std::string, std::vector, std::unordered_map, smart pointers, namespaces, const correctness; no heavy templates/metaprogramming)
- **Target platform:** Windows
- **Toolchain compatibility:** GCC / MinGW
- **Dependency policy:** Minimal external dependencies. Direct `g++` compile initially; CMake only when the project grows.

## 2. Core Philosophy

Runa must be: readable, consistent, relatively simple to learn, technically clean,
extensible, suitable for small scripts AND larger applications, implementable without
a huge runtime, and **distinct in syntax/identity** from Python, Java, C, C++,
JavaScript, Rust, Go, etc. No blind copying of other languages' syntax.

## 3. Confirmed Syntax (ONLY this much so far)

```runa
name = "Runa"
```

- Assignment operator: `=`
- Must remain unchanged unless the designer changes it.

## 4. Identifier Rules (intended, Unicode policy not finalized)

- May contain letters, digits, `_`
- Must NOT begin with a digit
- No whitespace / arbitrary punctuation
- Examples: `name`, `user_name`, `userName`, `name2`, `_name`
- ⚠ Unicode identifiers: DO NOT silently add support — ask the designer.

## 5. NOT YET DEFINED (do not invent without designer approval)

primitive types, arrays, lists, maps, tuples, objects, classes, structs, enums,
constants, functions & function syntax, parameters, returns, conditions, loops,
operators (logical/comparison), comments, imports, modules, packages, visibility,
error handling, exceptions, pattern matching, generics, interfaces, concurrency,
async, memory management, GC, FFI, native functions, decorators/annotations,
macros, metaprogramming, type system (static/dynamic/optional/inference),
optimizations, standard library API, package manager, debugger, formatter,
language server, documentation syntax.

## 6. Architecture (fixed pipeline)

```text
Runa Source (.ru) → Lexer → Tokens → Parser → AST → Compiler → Bytecode → VM → Runtime → OS/CPU
```

- Lexer: source → tokens. No semantic analysis. Token kinds only when syntax is defined.
- Parser: tokens → AST. Errors with file/line/column + source context.
- AST: logical structure only; independent from VM; no bytecode inside.
- Compiler: AST → Bytecode. Never executes. Separate from VM.
- Bytecode: intermediate representation for the Runa VM (not machine code).
  Binary format NOT finalized; don't lock format before instruction set stabilizes.
- VM: executes bytecode only. Never parses `.ru` directly. Stack-based initially.
- Runtime: object representation, strings, arrays, functions, memory, built-ins,
  native functions, runtime errors. Conceptually separate from VM.
- Stdlib areas (API not defined): io, strings, collections, math, files, system,
  time, network, process.

## 7. Errors (formatting not finalized)

Categories: LexicalError, SyntaxError, CompileError, RuntimeError.
Fields: filename, line, column, message.
Example: `main.ru:3:9  SyntaxError: unexpected token`

## 8. Planned Repository Layout

```text
Runa/
├── src/
│   ├── main.cpp
│   ├── lexer/    lexer.cpp   lexer.hpp
│   ├── parser/   parser.cpp  parser.hpp
│   ├── ast/      ast.cpp     ast.hpp
│   ├── compiler/ compiler.cpp compiler.hpp
│   ├── bytecode/ bytecode.cpp bytecode.hpp
│   ├── vm/       vm.cpp      vm.hpp
│   ├── runtime/  runtime.cpp runtime.hpp
│   ├── stdlib/   stdlib.cpp  stdlib.hpp
│   └── errors/   errors.cpp  errors.hpp
├── examples/
│   └── main.ru
└── build/
```

## 9. Executables (eventual)

- `runac.exe`  — Runa Compiler   (`main.ru → main.rbc`)
- `runavm.exe` — Runa VM         (`main.rbc → execution`)
- `runa.exe`   — combined CLI (optional, must not break compiler/VM separation)

## 10. Development Order (strict)

```text
1. Language syntax specification
2. Token definitions
3. Lexer
4. AST definitions
5. Parser
6. Bytecode specification
7. Compiler
8. VM
9. Runtime
10. Standard Library
11. CLI/tooling
12. Testing
13. Optimization
```

⚠ Never jump to bytecode/VM before syntax, Lexer, AST, Parser are sufficiently defined.

## 11. Testing Strategy

- Lexer tests:   source → expected tokens
- Parser tests:  tokens → expected AST
- Compiler tests: AST → expected bytecode
- VM tests:      bytecode → expected result
- Integration:   Runa source → final output

## 12. CURRENT DEVELOPMENT TARGET

**NOT** a complete language yet. Immediate target:

```text
Runa → Lexer → Parser → AST
```

starting ONLY with: `name = "Runa"`
Then expand incrementally. Clarity > looking advanced.

## 13. THE Design Rule (most important)

**The language designer decides Runa's syntax.**

If a feature is undefined:
1. identify the undefined feature
2. explain the relevant design choices
3. optionally propose alternatives
4. let the designer choose
5. record the decision
6. implement it

## 14. Hard Rules (do not violate)

- Do not assume unspecified syntax.
- Do not merge Compiler and VM.
- Do not change the `.ru` extension.
- Do not change `name = "Runa"` assignment syntax.
- Build incrementally; test every layer before the next.
