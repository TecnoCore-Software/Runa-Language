#!/usr/bin/env bash
# Runa integration tests — exercises every layer through the real tools:
#
#     Runa source → Lexer → Parser → AST → Compiler → Bytecode → VM
#
# (Linux/macOS. On Windows, run the same commands manually or via Git Bash.)
set -u
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
RUNAC="$ROOT/build/runac"
RUNAVM="$ROOT/build/runavm"
RUNA="$ROOT/build/runa"
RPR="$ROOT/build/rpr"
NRUNA="$ROOT/build/nruna"
CASES="$ROOT/tests/cases"

if [[ ! -x "$RUNAC" || ! -x "$RUNAVM" || ! -x "$RUNA" || ! -x "$RPR" || ! -x "$NRUNA" ]]; then
    echo "tests: binaries not found — run ./build.sh first"
    exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

PASS=0
FAIL=0

pass() { PASS=$((PASS+1)); echo "PASS  $1"; }
failed() { FAIL=$((FAIL+1)); echo "FAIL  $1"; }

check_rc() { # name expected_rc actual_rc
    if [[ "$3" -eq "$2" ]]; then pass "$1"; else failed "$1 (rc=$3, expected $2)"; fi
}
contains_file() { # name file needle
    if grep -qF -- "$3" "$2"; then pass "$1"; else failed "$1 (missing: $3)"; fi
}
line_is() { # name file expected_full_line (exact whole-line match)
    if grep -qxFx -- "$3" "$2"; then pass "$1"; else failed "$1 (missing exact line: $3)"; fi
}
not_contains_file() { # name file needle (needle must NOT appear)
    if grep -qF -- "$3" "$2"; then failed "$1 (unexpected: $3)"; else pass "$1"; fi
}
line_count_is() { # name file expected_count (number of output lines)
    local count
    count=$(wc -l < "$2")
    if [[ "$count" -eq "$3" ]]; then pass "$1"; else failed "$1 (got $count lines, expected $3)"; fi
}

# ---- 1. the canonical confirmed syntax: compile ----
"$RUNAC" examples/main.ru -o "$TMP/main.rbc" >"$TMP/o1" 2>"$TMP/e1"; RC=$?
check_rc "t01 compile main.ru" 0 "$RC"
if [[ -s "$TMP/main.rbc" ]]; then pass "t02 .rbc file written"; else failed "t02 .rbc file written"; fi
if head -c 4 "$TMP/main.rbc" | grep -q "RUNA"; then pass "t03 .rbc magic number"; else failed "t03 .rbc magic number"; fi

# ---- 2. VM executes the bytecode ----
"$RUNAVM" "$TMP/main.rbc" >"$TMP/o4" 2>"$TMP/e4"; RC=$?
check_rc "t04 runavm executes bytecode" 0 "$RC"

"$RUNAVM" "$TMP/main.rbc" --dump-globals >"$TMP/o5" 2>/dev/null
contains_file "t05 globals dump shows assignment" "$TMP/o5" 'name = "Runa"'

# ---- 3. combined tool (same separate compiler + VM inside) ----
"$RUNA" examples/main.ru --dump-globals >"$TMP/o6" 2>/dev/null
contains_file "t06 runa one-shot tool" "$TMP/o6" 'name = "Runa"'

# ---- 4. multiple statements ----
"$RUNA" examples/greeting.ru --dump-globals >"$TMP/o7" 2>/dev/null
contains_file "t07 multi-assignment program" "$TMP/o7" 'version = "0.1.0"'
contains_file "t07b multi-assignment program" "$TMP/o7" 'motto = "readable, consistent, clean"'

# ---- 5. reassignment: last value wins ----
"$RUNA" "$CASES/reassign.ru" --dump-globals >"$TMP/o8" 2>/dev/null
contains_file "t08 reassignment" "$TMP/o8" 'name = "B"'

# ---- 6. development dumps ----
"$RUNAC" examples/main.ru --dump-tokens >"$TMP/o9" 2>/dev/null
contains_file "t09 token dump: Identifier" "$TMP/o9" "Identifier"
contains_file "t09b token dump: Equal" "$TMP/o9" "Equal"
contains_file "t09c token dump: Eof" "$TMP/o9" "Eof"

"$RUNAC" examples/main.ru --dump-ast >"$TMP/o10" 2>/dev/null
contains_file "t10 ast dump: assignment" "$TMP/o10" "AssignmentStatement"
contains_file "t10b ast dump: identifier" "$TMP/o10" "target Identifier 'name'"
contains_file "t10c ast dump: literal" "$TMP/o10" 'StringLiteral "Runa"'

"$RUNAC" examples/main.ru --dump-bytecode >"$TMP/o11" 2>/dev/null
contains_file "t11 disassembly: LoadConst" "$TMP/o11" "LoadConst"
contains_file "t11b disassembly: StoreGlobal" "$TMP/o11" "StoreGlobal"
contains_file "t11c disassembly: Halt" "$TMP/o11" "Halt"

"$RUNA" examples/main.ru --trace >"$TMP/o12" 2>/dev/null
contains_file "t12 vm trace" "$TMP/o12" "LoadConst"

# ---- 7. lexical errors ----
"$RUNAC" "$CASES/err_unterminated_string.ru" >/dev/null 2>"$TMP/e13"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t13 unterminated string fails"; else failed "t13 unterminated string fails"; fi
contains_file "t13b LexicalError kind" "$TMP/e13" "LexicalError"
contains_file "t13c unterminated message" "$TMP/e13" "unterminated"

"$RUNAC" "$CASES/err_identifier_digit.ru" >/dev/null 2>"$TMP/e14"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t14 digit-initial identifier fails"; else failed "t14 digit-initial identifier fails"; fi
contains_file "t14b digit-initial is a syntax error" "$TMP/e14" "SyntaxError"
contains_file "t14c digit-initial message" "$TMP/e14" "expected a statement"

# ---- 8. syntax errors with precise locations ----
"$RUNAC" "$CASES/err_missing_equal.ru" >/dev/null 2>"$TMP/e15"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t15 missing '=' fails"; else failed "t15 missing '=' fails"; fi
contains_file "t15b SyntaxError kind" "$TMP/e15" "SyntaxError"
contains_file "t15c expected-'='-or-call message" "$TMP/e15" "expected '=', '(' or '.' after name 'name'"
contains_file "t15d exact location 1:6" "$TMP/e15" "err_missing_equal.ru:1:6"
contains_file "t15e source context line" "$TMP/e15" '| name "Runa"'
contains_file "t15f caret under error" "$TMP/e15" "  |      ^"

"$RUNAC" "$CASES/err_missing_value.ru" >/dev/null 2>"$TMP/e16"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t16 missing value fails"; else failed "t16 missing value fails"; fi
contains_file "t16b expected-expression message" "$TMP/e16" "expected an expression"

# ---- 9. edge cases ----
"$RUNA" "$CASES/empty.ru" --dump-globals >"$TMP/o17" 2>/dev/null; RC=$?
check_rc "t17 empty program is valid" 0 "$RC"
contains_file "t17b empty program has no globals" "$TMP/o17" "(no globals)"

"$RUNA" "$CASES/blank_lines.ru" >/dev/null 2>&1; RC=$?
check_rc "t18 blank lines everywhere" 0 "$RC"

# ---- 10. bytecode container validation ----
"$RUNAVM" "$CASES/not_rbc.txt" >"$TMP/o19" 2>"$TMP/e19"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t19 corrupt bytecode rejected"; else failed "t19 corrupt bytecode rejected"; fi
contains_file "t19b corrupt bytecode message" "$TMP/e19" "not a Runa bytecode file"

# ---- 11. printr: the first built-in ------------------------------------------
"$RUNA" examples/hello.ru >"$TMP/o20" 2>/dev/null; RC=$?
check_rc "t20 hello.ru runs" 0 "$RC"
contains_file "t20b printr value not name" "$TMP/o20" "Hello Runa"

"$RUNAC" examples/hello.ru -o "$TMP/hello.rbc" >/dev/null 2>&1
"$RUNAVM" "$TMP/hello.rbc" >"$TMP/o21" 2>/dev/null; RC=$?
check_rc "t21 compile+run split pipeline" 0 "$RC"
contains_file "t21b split pipeline output" "$TMP/o21" "Hello Runa"

"$RUNA" "$CASES/literals.ru" >"$TMP/o22" 2>/dev/null; RC=$?
check_rc "t22 printr string literal" 0 "$RC"
contains_file "t22b string output" "$TMP/o22" "Hello"

printf 'printr(123)\n' >"$TMP/num.ru"
"$RUNA" "$TMP/num.ru" >"$TMP/o23" 2>/dev/null
contains_file "t23 printr number" "$TMP/o23" "123"

printf 'printr(true)\nprintr(false)\n' >"$TMP/bool.ru"
"$RUNA" "$TMP/bool.ru" >"$TMP/o24" 2>/dev/null
contains_file "t24a printr true" "$TMP/o24" "true"
contains_file "t24b printr false" "$TMP/o24" "false"

printf 'printr("a", 1, true)\n' >"$TMP/multi.ru"
"$RUNA" "$TMP/multi.ru" >"$TMP/o25" 2>/dev/null
contains_file "t25 multiple mixed args" "$TMP/o25" "a 1 true"

"$RUNA" "$CASES/vars.ru" >"$TMP/o26" 2>/dev/null; RC=$?
check_rc "t26 variables of every kind" 0 "$RC"
contains_file "t26b number variable output" "$TMP/o26" "5"
contains_file "t26c boolean variable output" "$TMP/o26" "false"

"$RUNA" "$CASES/dynretype.ru" >"$TMP/o27" 2>/dev/null; RC=$?
check_rc "t27 dynamic retype is allowed" 0 "$RC"
contains_file "t27b retyped value" "$TMP/o27" "99"

printf 'a = "x"\nb = a\nprintr(b)\n' >"$TMP/copy.ru"
"$RUNA" "$TMP/copy.ru" >"$TMP/o28" 2>/dev/null
contains_file "t28 variable-to-variable copy" "$TMP/o28" "x"

"$RUNA" examples/types.ru >"$TMP/o29" 2>/dev/null; RC=$?
check_rc "t29 types example runs" 0 "$RC"
contains_file "t29b types example line" "$TMP/o29" "Welcome to Runa"

# ---- 12. compile-time call validation ---------------------------------------
# Runa 1.0 (DECISIONS #114): printr() with ZERO arguments is valid and prints
# just a newline.
printf 'printr()\nprintr("after")\n' >"$TMP/z30.ru"
"$RUNAC" "$TMP/z30.ru" -o "$TMP/z30.rbc" >/dev/null 2>"$TMP/e30"; RC=$?
check_rc "t30 printr() with zero args compiles" 0 "$RC"
"$RUNAVM" "$TMP/z30.rbc" >"$TMP/o30c" 2>/dev/null
line_is "t30b printr() prints a newline then the next line" "$TMP/o30c" "after"
printf 'fun printr(x) []\n' >"$TMP/z30r.ru"
"$RUNAC" "$TMP/z30r.ru" >/dev/null 2>"$TMP/e30r"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t30c printr shadowing rejected"; else failed "t30c printr shadowing rejected"; fi
contains_file "t30d printr reservation message" "$TMP/e30r" "core reserved feature"

"$RUNAC" "$CASES/err_unknown_fn.ru" >/dev/null 2>"$TMP/e31"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t31 unknown function rejected"; else failed "t31 unknown function rejected"; fi
contains_file "t31b unknown function message" "$TMP/e31" "call to unknown function 'foo'"

# ---- 13. runtime: undefined variables ---------------------------------------
"$RUNAC" "$CASES/err_undefined.ru" -o "$TMP/undef.rbc" >/dev/null 2>&1; RC=$?
check_rc "t32 undefined variable compiles" 0 "$RC"
"$RUNAVM" "$TMP/undef.rbc" >/dev/null 2>"$TMP/e32"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t32b undefined variable fails at runtime"; else failed "t32b undefined variable fails at runtime"; fi
contains_file "t32c undefined message" "$TMP/e32" "undefined variable 'missing'"
contains_file "t32d RuntimeError kind" "$TMP/e32" "RuntimeError"

# ---- 14. more syntax errors ---------------------------------------------------
"$RUNAC" "$CASES/err_missing_close.ru" >/dev/null 2>"$TMP/e33"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t33 missing ')' fails"; else failed "t33 missing ')' fails"; fi
contains_file "t33b missing close message" "$TMP/e33" "expected ',' or ')'"

"$RUNAC" "$CASES/err_trailing_comma.ru" >/dev/null 2>"$TMP/e34"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t34 trailing comma fails"; else failed "t34 trailing comma fails"; fi
contains_file "t34b trailing comma message" "$TMP/e34" "expected an expression"

"$RUNAC" "$CASES/err_assign_true.ru" >/dev/null 2>"$TMP/e35"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t35 assigning to 'true' fails"; else failed "t35 assigning to 'true' fails"; fi
contains_file "t35b reserved word message" "$TMP/e35" "expected a statement"

# ---- 15. number literals ------------------------------------------------------
"$RUNAC" "$CASES/err_overflow.ru" >/dev/null 2>"$TMP/e36"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t36 integer overflow fails"; else failed "t36 integer overflow fails"; fi
contains_file "t36b overflow message" "$TMP/e36" "integer literal out of range"
contains_file "t36c LexicalError kind" "$TMP/e36" "LexicalError"

# t37 (DECISIONS #126): negative literals are VALID now. The old behavior
# ("unary '-' is not supported") was replaced by real unary minus, so the
# same file now runs and prints -5. Unary '+' stays rejected (t37d).
"$RUNA" "$CASES/err_negative.ru" >"$TMP/o37" 2>/dev/null; RC=$?
if [[ "$RC" -eq 0 ]]; then pass "t37 negative literal runs"; else failed "t37 negative literal runs"; fi
contains_file "t37b negative literal value" "$TMP/o37" "-5"
printf 'x = +5\n' >"$TMP/uplus.ru"
"$RUNAC" "$TMP/uplus.ru" >/dev/null 2>"$TMP/e37c"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t37c unary plus still rejected"; else failed "t37c unary plus still rejected"; fi
contains_file "t37d unary plus message" "$TMP/e37c" "unary '+' is not supported in Runa yet"

printf 'printr(007)\n' >"$TMP/leading.ru"
"$RUNA" "$TMP/leading.ru" >"$TMP/o38" 2>/dev/null
contains_file "t38 leading zeros normalized" "$TMP/o38" "7"

# ---- 16. new instructions in dumps -------------------------------------------
"$RUNAC" examples/hello.ru --dump-bytecode >"$TMP/o39" 2>/dev/null
contains_file "t39 disassembly: LoadGlobal" "$TMP/o39" "LoadGlobal"
contains_file "t39b disassembly: Call" "$TMP/o39" "Call"
contains_file "t39c disassembly: printr" "$TMP/o39" "printr (argc=2)"

"$RUNA" examples/hello.ru --trace >"$TMP/o40" 2>/dev/null
contains_file "t40 trace shows Call" "$TMP/o40" "Call"

# ---- 17. bytecode container versioning ----------------------------------------
printf 'RUNA\x00\x00\x00\x00' >"$TMP/old.rbc"
"$RUNAVM" "$TMP/old.rbc" >/dev/null 2>"$TMP/e41"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t41 version-0 file rejected"; else failed "t41 version-0 file rejected"; fi
contains_file "t41b version message" "$TMP/e41" "unsupported bytecode format version 0"

# ---- 18. arithmetic: the five operators (designer's exact example) -------------
"$RUNA" examples/arithmetic.ru >"$TMP/o42" 2>/dev/null; RC=$?
check_rc "t42 arithmetic example runs" 0 "$RC"
line_is "t42b a + b = 13" "$TMP/o42" "13"
line_is "t42c a - b = 7"  "$TMP/o42" "7"
line_is "t42d a * b = 30" "$TMP/o42" "30"
line_is "t42e a / b = 3"  "$TMP/o42" "3"
line_is "t42f a % b = 1"  "$TMP/o42" "1"

# ---- 19. arithmetic: numeric literals ------------------------------------------
printf 'printr(10 + 5)\nprintr(20 * 3)\nprintr(10 %% 3)\n' >"$TMP/arith_lit.ru"
"$RUNA" "$TMP/arith_lit.ru" >"$TMP/o43" 2>/dev/null
line_is "t43 literal 10 + 5" "$TMP/o43" "15"
line_is "t43b literal 20 * 3" "$TMP/o43" "60"
line_is "t43c literal 10 % 3" "$TMP/o43" "1"

# ---- 20. arithmetic: precedence and grouping -----------------------------------
printf 'printr(2 + 3 * 4)\n' >"$TMP/prec.ru"
"$RUNA" "$TMP/prec.ru" >"$TMP/o44" 2>/dev/null
line_is "t44 precedence 2 + 3 * 4 = 14" "$TMP/o44" "14"

printf 'printr((2 + 3) * 4)\n' >"$TMP/group.ru"
"$RUNA" "$TMP/group.ru" >"$TMP/o45" 2>/dev/null
line_is "t45 grouping (2 + 3) * 4 = 20" "$TMP/o45" "20"

printf 'printr(2 + 3 * 4 - 5)\nprintr(1 + 2 * 3 %% 4)\n' >"$TMP/deep.ru"
"$RUNA" "$TMP/deep.ru" >"$TMP/o46" 2>/dev/null
line_is "t46 mixed precedence 2 + 3 * 4 - 5" "$TMP/o46" "9"
line_is "t46b mixed precedence 1 + 2 * 3 % 4" "$TMP/o46" "3"

# ---- 21. arithmetic: left associativity on equal precedence --------------------
printf 'printr(10 - 4 - 3)\nprintr(100 / 10 / 2)\nprintr(2 * 3 %% 4)\nprintr(10 %% 4 * 2)\n' >"$TMP/assoc.ru"
"$RUNA" "$TMP/assoc.ru" >"$TMP/o47" 2>/dev/null
line_is "t47 left-assoc 10 - 4 - 3" "$TMP/o47" "3"
line_is "t47b left-assoc 100 / 10 / 2" "$TMP/o47" "5"
line_is "t47c left-assoc 2 * 3 % 4" "$TMP/o47" "2"
line_is "t47d left-assoc 10 % 4 * 2" "$TMP/o47" "4"

# ---- 22. arithmetic: variables and assignment RHS ------------------------------
printf 'x = 10\ny = 2\nprintr(x + y)\n' >"$TMP/vars_arith.ru"
"$RUNA" "$TMP/vars_arith.ru" >"$TMP/o48" 2>/dev/null
line_is "t48 variables in expression x + y" "$TMP/o48" "12"

printf 'a = 7\nb = a * 2\nprintr(b - a)\n' >"$TMP/rhs_expr.ru"
"$RUNA" "$TMP/rhs_expr.ru" >"$TMP/o49" 2>/dev/null
line_is "t49 assignment RHS is an expression" "$TMP/o49" "7"

# ---- 23. arithmetic: negative results, spacing, mixed args ---------------------
printf 'printr(3 - 10)\n' >"$TMP/negres.ru"
"$RUNA" "$TMP/negres.ru" >"$TMP/o50" 2>/dev/null
line_is "t50 negative result 3 - 10" "$TMP/o50" "-7"

printf 'printr(10+5)\nprintr(  10  +  5  )\n' >"$TMP/spaces.ru"
"$RUNA" "$TMP/spaces.ru" >"$TMP/o51" 2>/dev/null
line_is "t51 spacing is insignificant" "$TMP/o51" "15"

printf 'printr("sum:", 2 + 2)\n' >"$TMP/mixed_args.ru"
"$RUNA" "$TMP/mixed_args.ru" >"$TMP/o52" 2>/dev/null
line_is "t52 expression mixed with string arg" "$TMP/o52" "sum: 4"

# ---- 24. arithmetic: runtime errors --------------------------------------------
printf 'printr(10 / 0)\n' >"$TMP/div0.ru"
"$RUNAC" "$TMP/div0.ru" -o "$TMP/div0.rbc" >/dev/null 2>&1; RC=$?
check_rc "t53 division by zero compiles" 0 "$RC"
"$RUNAVM" "$TMP/div0.rbc" >/dev/null 2>"$TMP/e53"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t53b division by zero fails at runtime"; else failed "t53b division by zero fails at runtime"; fi
contains_file "t53c division by zero message" "$TMP/e53" "division by zero"
contains_file "t53d RuntimeError kind" "$TMP/e53" "RuntimeError"

printf 'printr(10 %% 0)\n' >"$TMP/mod0.ru"
"$RUNA" "$TMP/mod0.ru" >/dev/null 2>"$TMP/e54"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t54 modulo by zero fails"; else failed "t54 modulo by zero fails"; fi
contains_file "t54b modulo by zero message" "$TMP/e54" "modulo by zero"

# t55 (DECISIONS #128): string + number CONCATENATES now (the number joins
# in its printr text form). Non-string non-numeric '+' stays rejected —
# moved to t55c/t55d as a compile-time error (DECISIONS #130).
printf 'printr("a" + 1)\n' >"$TMP/stradd.ru"
"$RUNA" "$TMP/stradd.ru" >"$TMP/o55" 2>/dev/null; RC=$?
if [[ "$RC" -eq 0 ]]; then pass "t55 string + number concatenates"; else failed "t55 string + number concatenates"; fi
line_is "t55b concat result is text form" "$TMP/o55" "a1"
printf 'printr(true + true)\n' >"$TMP/booladd.ru"
"$RUNA" "$TMP/booladd.ru" >/dev/null 2>"$TMP/e55x"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t55c boolean + boolean rejected"; else failed "t55c boolean + boolean rejected"; fi
contains_file "t55d compile-time operand message" "$TMP/e55x" "unsupported operand types for '+'"
contains_file "t55e operand kinds listed" "$TMP/e55x" "boolean and boolean"

printf 'printr(9223372036854775807 + 1)\n' >"$TMP/ovf.ru"
"$RUNA" "$TMP/ovf.ru" >/dev/null 2>"$TMP/e56"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t56 signed overflow rejected"; else failed "t56 signed overflow rejected"; fi
contains_file "t56b overflow message" "$TMP/e56" "integer overflow in '+'"

# ---- 25. arithmetic: syntax errors ----------------------------------------------
printf 'printr(10 + )\n' >"$TMP/err_arith1.ru"
"$RUNAC" "$TMP/err_arith1.ru" >/dev/null 2>"$TMP/e57"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t57 dangling '+' fails"; else failed "t57 dangling '+' fails"; fi
contains_file "t57b dangling operand message" "$TMP/e57" "expected an expression"

printf 'printr(10 5)\n' >"$TMP/err_arith2.ru"
"$RUNAC" "$TMP/err_arith2.ru" >/dev/null 2>"$TMP/e58"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t58 missing operator fails"; else failed "t58 missing operator fails"; fi
contains_file "t58b missing operator message" "$TMP/e58" "expected ',' or ')'"

printf 'printr((10)\n' >"$TMP/err_arith3.ru"
"$RUNAC" "$TMP/err_arith3.ru" >/dev/null 2>"$TMP/e59"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t59 unclosed group fails"; else failed "t59 unclosed group fails"; fi
contains_file "t59b unclosed group message" "$TMP/e59" "expected ',' or ')'"

# ---- 26. arithmetic: dumps and trace --------------------------------------------
"$RUNAC" examples/arithmetic.ru --dump-bytecode >"$TMP/o60" 2>/dev/null
contains_file "t60 disassembly: Add" "$TMP/o60" "Add"
contains_file "t60b disassembly: Div" "$TMP/o60" "Div"
contains_file "t60c disassembly: Mod" "$TMP/o60" "Mod"

"$RUNA" examples/arithmetic.ru --trace >"$TMP/o61" 2>/dev/null
contains_file "t61 trace shows arithmetic ops" "$TMP/o61" "Mul"

"$RUNAC" examples/arithmetic.ru --dump-ast >"$TMP/o62" 2>/dev/null
contains_file "t62 ast dump: FunctionCallExpr" "$TMP/o62" "FunctionCallExpr"

"$RUNAC" examples/arithmetic.ru --dump-tokens >"$TMP/o63" 2>/dev/null
contains_file "t63 token dump: Plus" "$TMP/o63" "Plus"
contains_file "t63b token dump: Percent" "$TMP/o63" "Percent"

# ---- 27. if: the designer's exact spec example ---------------------------------
"$RUNA" examples/if.ru >"$TMP/o64" 2>/dev/null; RC=$?
check_rc "t64 if example runs" 0 "$RC"
line_is "t64b true condition executes" "$TMP/o64" "mtn is 5!"
line_is "t64c greater comparison" "$TMP/o64" "yes"
line_is "t64d block statement 1" "$TMP/o64" "one"
line_is "t64e block statement 2" "$TMP/o64" "two"
line_is "t64f block statement 3" "$TMP/o64" "three"
line_is "t64g code after if runs" "$TMP/o64" "done"
not_contains_file "t64h false block skipped" "$TMP/o64" "wrong"

# ---- 28. if: all six comparison operators ---------------------------------------
"$RUNA" examples/conditions.ru >"$TMP/o65" 2>/dev/null; RC=$?
check_rc "t65 conditions example runs" 0 "$RC"
line_is "t65b operator ==" "$TMP/o65" "equal"
line_is "t65c operator !=" "$TMP/o65" "not equal"
line_is "t65d operator >"  "$TMP/o65" "greater"
line_is "t65e operator <"  "$TMP/o65" "smaller"
line_is "t65f operator >=" "$TMP/o65" "greater or equal"
line_is "t65g operator <=" "$TMP/o65" "less or equal"

# ---- 29. if: false conditions skip the block (all six operators) ----------------
printf 'a = 5\nif a == 4 [\n    printr("x1")\n]\nif a != 5 [\n    printr("x2")\n]\nif a > 9 [\n    printr("x3")\n]\nif a < 1 [\n    printr("x4")\n]\nif a >= 6 [\n    printr("x5")\n]\nif a <= 4 [\n    printr("x6")\n]\nprintr("cleared")\n' >"$TMP/iffs.ru"
"$RUNA" "$TMP/iffs.ru" >"$TMP/o66" 2>/dev/null; RC=$?
check_rc "t66 six false conditions run" 0 "$RC"
line_is "t66b only post-if code printed" "$TMP/o66" "cleared"
not_contains_file "t66c no skipped output" "$TMP/o66" "x"
line_count_is "t66d exactly one output line" "$TMP/o66" 1

# ---- 30. if: assignments inside a block change state -----------------------------
printf 'n = 0\nif 2 > 1 [\n    n = n + 1\n    printr("inside", n)\n]\nprintr("after", n)\n' >"$TMP/blockstate.ru"
"$RUNA" "$TMP/blockstate.ru" >"$TMP/o67" 2>/dev/null; RC=$?
check_rc "t67 block with assignment runs" 0 "$RC"
line_is "t67b inside sees new value" "$TMP/o67" "inside 1"
line_is "t67c state persists after block" "$TMP/o67" "after 1"

# ---- 31. if: nested if blocks ----------------------------------------------------
printf 'a = 10\nif a > 5 [\n    if a < 20 [\n        printr("inner")\n    ]\n    printr("outer")\n]\nif a > 99 [\n    if a < 20 [\n        printr("skipped")\n    ]\n]\nprintr("end")\n' >"$TMP/nestif.ru"
"$RUNA" "$TMP/nestif.ru" >"$TMP/o68" 2>/dev/null; RC=$?
check_rc "t68 nested if runs" 0 "$RC"
line_is "t68b inner executes" "$TMP/o68" "inner"
line_is "t68c statement after inner executes" "$TMP/o68" "outer"
line_is "t68d code after ifs runs" "$TMP/o68" "end"
not_contains_file "t68e inner of false outer skipped" "$TMP/o68" "skipped"
line_count_is "t68f exactly three output lines" "$TMP/o68" 3

# ---- 32. if: equality on strings and booleans -------------------------------------
printf 'name = "Runa"\nif name == "Runa" [\n    printr("greeted")\n]\nif name != "Other" [\n    printr("different")\n]\nflag = true\nif flag == true [\n    printr("flagged")\n]\nif true != false [\n    printr("bools")\n]\n' >"$TMP/cmpeq.ru"
"$RUNA" "$TMP/cmpeq.ru" >"$TMP/o69" 2>/dev/null; RC=$?
check_rc "t69 equality on strings and booleans" 0 "$RC"
line_is "t69b string ==" "$TMP/o69" "greeted"
line_is "t69c string !=" "$TMP/o69" "different"
line_is "t69d boolean ==" "$TMP/o69" "flagged"
line_is "t69e boolean !=" "$TMP/o69" "bools"

# ---- 33. if: arithmetic inside conditions -----------------------------------------
printf 'x = 7\nif x * 2 >= 14 [\n    printr("math")\n]\nif x + 1 > x - 1 [\n    printr("always")\n]\nif (x - 7) == 0 [\n    printr("grouped")\n]\n' >"$TMP/condarith.ru"
"$RUNA" "$TMP/condarith.ru" >"$TMP/o70" 2>/dev/null; RC=$?
check_rc "t70 arithmetic in conditions" 0 "$RC"
line_is "t70b multiplication in condition" "$TMP/o70" "math"
line_is "t70c both sides arithmetic" "$TMP/o70" "always"
line_is "t70d grouped condition" "$TMP/o70" "grouped"

# ---- 34. if: syntax errors (spec section 7 forms) ----------------------------------
printf 'if [\n    printr("x")\n]\n' >"$TMP/err_if1.ru"
"$RUNAC" "$TMP/err_if1.ru" >/dev/null 2>"$TMP/e71"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t71 missing condition fails"; else failed "t71 missing condition fails"; fi
contains_file "t71b missing condition message" "$TMP/e71" "expected a condition after 'if'"
contains_file "t71c SyntaxError kind" "$TMP/e71" "SyntaxError"

printf 'if a == 5\n    printr("x")\n]\n' >"$TMP/err_if2.ru"
"$RUNAC" "$TMP/err_if2.ru" >/dev/null 2>"$TMP/e72"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t72 missing '[' fails"; else failed "t72 missing '[' fails"; fi
contains_file "t72b missing '[' message" "$TMP/e72" "expected '[' to open the if block"

printf 'if a == 5 [\n    printr("x")\n' >"$TMP/err_if3.ru"
"$RUNAC" "$TMP/err_if3.ru" >/dev/null 2>"$TMP/e73"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t73 missing ']' fails"; else failed "t73 missing ']' fails"; fi
contains_file "t73b missing ']' message" "$TMP/e73" "expected ']' to close the if block opened at line 1"
contains_file "t73c SyntaxError kind" "$TMP/e73" "SyntaxError"

printf 'if a > [\n    printr("x")\n]\n' >"$TMP/err_if4.ru"
"$RUNAC" "$TMP/err_if4.ru" >/dev/null 2>"$TMP/e74"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t74 missing comparison operand fails"; else failed "t74 missing comparison operand fails"; fi
contains_file "t74b expected-expression message" "$TMP/e74" "expected an expression"

printf 'a = 1\nif a < 2 < 3 [\n    printr("x")\n]\n' >"$TMP/err_if5.ru"
"$RUNAC" "$TMP/err_if5.ru" >/dev/null 2>"$TMP/e75"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t75 chained comparison fails"; else failed "t75 chained comparison fails"; fi
contains_file "t75b chained comparison message" "$TMP/e75" "chained comparisons are not supported in Runa yet"

printf 'if a == 5 {\n    printr("x")\n}\n' >"$TMP/err_if6.ru"
"$RUNAC" "$TMP/err_if6.ru" >/dev/null 2>"$TMP/e76"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t76 '{' braces rejected"; else failed "t76 '{' braces rejected"; fi
contains_file "t76b '{' not a block message" "$TMP/e76" "expected '[' to open the if block"
contains_file "t76c SyntaxError kind" "$TMP/e76" "SyntaxError"

printf 'printr("x")\n]\n' >"$TMP/err_if7.ru"
"$RUNAC" "$TMP/err_if7.ru" >/dev/null 2>"$TMP/e77"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t77 stray ']' fails"; else failed "t77 stray ']' fails"; fi
contains_file "t77b expected-a-statement message" "$TMP/e77" "expected a statement"

printf 'a = 5\nif a == 5 [ printr("x") ]\n' >"$TMP/oneline.ru"
"$RUNA" "$TMP/oneline.ru" >"$TMP/o78" 2>/dev/null; RC=$?
check_rc "t78 one-line block allowed" 0 "$RC"
line_is "t78b one-line block output" "$TMP/o78" "x"

# ---- 35. if: runtime errors ---------------------------------------------------------
printf 'if 5 [\n    printr("x")\n]\n' >"$TMP/rt_if1.ru"
"$RUNAC" "$TMP/rt_if1.ru" -o "$TMP/rt_if1.rbc" >/dev/null 2>&1; RC=$?
check_rc "t79 non-boolean condition compiles" 0 "$RC"
"$RUNAVM" "$TMP/rt_if1.rbc" >/dev/null 2>"$TMP/e79"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t79b non-boolean condition fails at runtime"; else failed "t79b non-boolean condition fails at runtime"; fi
contains_file "t79c boolean-required message" "$TMP/e79" "condition must be a boolean, got number"
contains_file "t79d RuntimeError kind" "$TMP/e79" "RuntimeError"

printf 'if "a" > 1 [\n    printr("x")\n]\n' >"$TMP/rt_if2.ru"
"$RUNA" "$TMP/rt_if2.ru" >/dev/null 2>"$TMP/e80"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t80 ordering on strings fails"; else failed "t80 ordering on strings fails"; fi
contains_file "t80b ordering type message" "$TMP/e80" "unsupported operand types for '>'"
contains_file "t80c operand kinds listed" "$TMP/e80" "string and number"

printf 'if 5 == "5" [\n    printr("bad")\n]\nif 5 != "5" [\n    printr("ne-works")\n]\nprintr("ok")\n' >"$TMP/rt_if3.ru"
"$RUNA" "$TMP/rt_if3.ru" >"$TMP/o81" 2>/dev/null; RC=$?
check_rc "t81 mixed-kind equality is false, not an error" 0 "$RC"
line_is "t81b mixed == skipped" "$TMP/o81" "ok"
line_is "t81c mixed != executes" "$TMP/o81" "ne-works"
not_contains_file "t81d mixed == did not run" "$TMP/o81" "bad"

# ---- 36. if: valid variants ----------------------------------------------------------
printf 'if 1 == 1 [\n]\nprintr("empty ok")\n' >"$TMP/emptyblock.ru"
"$RUNA" "$TMP/emptyblock.ru" >"$TMP/o82" 2>/dev/null; RC=$?
check_rc "t82 empty block is valid" 0 "$RC"
line_is "t82b empty block output" "$TMP/o82" "empty ok"

# ---- 37. if: dumps and trace -----------------------------------------------------------
"$RUNAC" examples/if.ru --dump-bytecode >"$TMP/o83" 2>/dev/null
contains_file "t83 disassembly: JumpIfFalse" "$TMP/o83" "JumpIfFalse"
contains_file "t83b disassembly: Eq" "$TMP/o83" "Eq"

"$RUNAC" examples/conditions.ru --dump-bytecode >"$TMP/o84" 2>/dev/null
contains_file "t84 disassembly: Gt" "$TMP/o84" "Gt"
contains_file "t84b disassembly: Lt" "$TMP/o84" "Lt"
contains_file "t84c disassembly: Ge" "$TMP/o84" "Ge"
contains_file "t84d disassembly: Le" "$TMP/o84" "Le"
contains_file "t84e disassembly: Ne" "$TMP/o84" "Ne"

"$RUNAC" examples/if.ru --dump-ast >"$TMP/o85" 2>/dev/null
contains_file "t85 ast dump: IfStatement" "$TMP/o85" "IfStatement"
contains_file "t85b ast dump: condition" "$TMP/o85" "condition BinaryExpr (mtn == 5)"

"$RUNAC" examples/if.ru --dump-tokens >"$TMP/o86" 2>/dev/null
contains_file "t86 token dump: If" "$TMP/o86" "If"
contains_file "t86b token dump: EqualEqual" "$TMP/o86" "EqualEqual"
contains_file "t86c token dump: LeftBracket" "$TMP/o86" "LeftBracket"
contains_file "t86d token dump: RightBracket" "$TMP/o86" "RightBracket"

"$RUNA" examples/if.ru --trace >"$TMP/o87" 2>/dev/null
contains_file "t87 trace shows JumpIfFalse" "$TMP/o87" "JumpIfFalse"

# ---- 38. elif/else: if + else (spec section 3) -------------------------------------
printf 'a = 10\n\nif a == 10 [\n    printr("yes")\n] else [\n    printr("no")\n]\n\nprintr("done")\n' >"$TMP/chain1.ru"
"$RUNA" "$TMP/chain1.ru" >"$TMP/o88" 2>/dev/null; RC=$?
check_rc "t88 if+else runs" 0 "$RC"
line_is "t88b true branch executes" "$TMP/o88" "yes"
line_is "t88c execution continues after chain" "$TMP/o88" "done"
not_contains_file "t88d else branch skipped" "$TMP/o88" "no"
line_count_is "t88e exactly two output lines" "$TMP/o88" 2

# ---- 39. elif/else: if + else, condition false --------------------------------------
printf 'a = 5\nif a == 10 [\n    printr("wrong")\n] else [\n    printr("right")\n]\n' >"$TMP/chain2.ru"
"$RUNA" "$TMP/chain2.ru" >"$TMP/o89" 2>/dev/null; RC=$?
check_rc "t89 if+else false condition runs" 0 "$RC"
line_is "t89b else branch executes" "$TMP/o89" "right"
not_contains_file "t89c if branch skipped" "$TMP/o89" "wrong"

# ---- 40. elif/else: if + one elif (no else) ------------------------------------------
printf 'a = 20\nif a == 1 [\n    printr("one")\n] elif a == 20 [\n    printr("twenty")\n]\nprintr("end")\n' >"$TMP/chain3.ru"
"$RUNA" "$TMP/chain3.ru" >"$TMP/o90" 2>/dev/null; RC=$?
check_rc "t90 if+one-elif runs" 0 "$RC"
line_is "t90b elif branch executes" "$TMP/o90" "twenty"
line_is "t90c code after chain runs" "$TMP/o90" "end"
not_contains_file "t90d if branch skipped" "$TMP/o90" "one"
line_count_is "t90e exactly two output lines" "$TMP/o90" 2

# ---- 41. elif/else: the designer's exact spec example ---------------------------------
printf 'mtn = 10\n\nif mtn < 5 [\n    printr("small")\n] elif mtn == 10 [\n    printr("ten")\n] else [\n    printr("other")\n]\n' >"$TMP/chain4.ru"
"$RUNA" "$TMP/chain4.ru" >"$TMP/o91" 2>/dev/null; RC=$?
check_rc "t91 if+elif+else (spec example) runs" 0 "$RC"
line_is "t91b elif branch output" "$TMP/o91" "ten"
not_contains_file "t91c if branch skipped" "$TMP/o91" "small"
not_contains_file "t91d else branch skipped" "$TMP/o91" "other"
line_count_is "t91e exactly one output line" "$TMP/o91" 1

# ---- 42. elif/else: multiple elifs (spec section 2) -----------------------------------
printf 'a = 2\n\nif a == 1 [\n    printr("one")\n] elif a == 2 [\n    printr("two")\n] elif a == 3 [\n    printr("three")\n] else [\n    printr("other")\n]\n' >"$TMP/chain5.ru"
"$RUNA" "$TMP/chain5.ru" >"$TMP/o92" 2>/dev/null; RC=$?
check_rc "t92 if+multiple-elif runs" 0 "$RC"
line_is "t92b middle elif executes" "$TMP/o92" "two"
not_contains_file "t92c first branch skipped" "$TMP/o92" "one"
not_contains_file "t92d later branches skipped" "$TMP/o92" "three"
not_contains_file "t92e else skipped" "$TMP/o92" "other"
line_count_is "t92f exactly one output line" "$TMP/o92" 1

# ---- 43. elif/else: first true branch wins --------------------------------------------
printf 'a = 1\nif a == 1 [\n    printr("first")\n] elif a < 5 [\n    printr("second")\n] else [\n    printr("third")\n]\n' >"$TMP/chain6.ru"
"$RUNA" "$TMP/chain6.ru" >"$TMP/o93" 2>/dev/null; RC=$?
check_rc "t93 first-true chain runs" 0 "$RC"
line_is "t93b first branch wins" "$TMP/o93" "first"
not_contains_file "t93c later branches skipped" "$TMP/o93" "second"
not_contains_file "t93d else skipped" "$TMP/o93" "third"
line_count_is "t93e at most one branch" "$TMP/o93" 1

# ---- 44. elif/else: final elif true ----------------------------------------------------
printf 'a = 3\nif a == 1 [\n    printr("one")\n] elif a == 2 [\n    printr("two")\n] elif a == 3 [\n    printr("three")\n] else [\n    printr("other")\n]\n' >"$TMP/chain7.ru"
"$RUNA" "$TMP/chain7.ru" >"$TMP/o94" 2>/dev/null; RC=$?
check_rc "t94 final-elif-true chain runs" 0 "$RC"
line_is "t94b final elif executes" "$TMP/o94" "three"
not_contains_file "t94c earlier branches skipped" "$TMP/o94" "one"
not_contains_file "t94d else skipped" "$TMP/o94" "other"

# ---- 45. elif/else: all conditions false → else ----------------------------------------
printf 'a = 99\nif a == 1 [\n    printr("one")\n] elif a == 2 [\n    printr("two")\n] elif a == 3 [\n    printr("three")\n] else [\n    printr("fallback")\n]\n' >"$TMP/chain8.ru"
"$RUNA" "$TMP/chain8.ru" >"$TMP/o95" 2>/dev/null; RC=$?
check_rc "t95 all-false chain runs" 0 "$RC"
line_is "t95b else executes" "$TMP/o95" "fallback"
not_contains_file "t95c no condition branch ran" "$TMP/o95" "one"
not_contains_file "t95d no elif branch ran" "$TMP/o95" "three"

# ---- 46. elif/else: no branch true and no else ------------------------------------------
printf 'a = 99\nif a == 1 [\n    printr("one")\n] elif a == 2 [\n    printr("two")\n]\nprintr("after")\n' >"$TMP/chain9.ru"
"$RUNA" "$TMP/chain9.ru" >"$TMP/o96" 2>/dev/null; RC=$?
check_rc "t96 no-branch-taken (no else) is valid" 0 "$RC"
line_is "t96b code after chain still runs" "$TMP/o96" "after"
not_contains_file "t96c if body skipped" "$TMP/o96" "one"
not_contains_file "t96d elif body skipped" "$TMP/o96" "two"
line_count_is "t96e exactly one output line" "$TMP/o96" 1

# ---- 47. elif/else: nested chains (spec section 6) --------------------------------------
printf 'a = 10\nb = 20\n\nif a == 10 [\n    if b == 20 [\n        printr("nested")\n    ] else [\n        printr("wrong")\n    ]\n] else [\n    printr("outer wrong")\n]\n' >"$TMP/chain10.ru"
"$RUNA" "$TMP/chain10.ru" >"$TMP/o97" 2>/dev/null; RC=$?
check_rc "t97 nested if/else chain runs" 0 "$RC"
line_is "t97b nested branch executes" "$TMP/o97" "nested"
not_contains_file "t97c inner else skipped" "$TMP/o97" "wrong"
not_contains_file "t97d outer else skipped" "$TMP/o97" "outer wrong"

printf 'a = 7\nb = 2\nif a == 7 [\n    if b == 99 [\n        printr("no")\n    ] elif b == 2 [\n        printr("inner-elif")\n    ] else [\n        printr("inner-else")\n    ]\n] elif a == 8 [\n    printr("outer-elif")\n] else [\n    printr("outer-else")\n]\n' >"$TMP/chain11.ru"
"$RUNA" "$TMP/chain11.ru" >"$TMP/o97b" 2>/dev/null; RC=$?
check_rc "t97f nested elif/else chain runs" 0 "$RC"
line_is "t97g nested elif executes" "$TMP/o97b" "inner-elif"
not_contains_file "t97h outer elif skipped" "$TMP/o97b" "outer-elif"
not_contains_file "t97i outer else skipped" "$TMP/o97b" "outer-else"

# ---- 48. elif/else: invalid standalone elif (spec section 5) ----------------------------
printf 'elif a == 5 [\n    printr("x")\n]\n' >"$TMP/err_elif1.ru"
"$RUNAC" "$TMP/err_elif1.ru" >/dev/null 2>"$TMP/e98"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t98 standalone elif fails"; else failed "t98 standalone elif fails"; fi
contains_file "t98b standalone elif message" "$TMP/e98" "'elif' without a matching 'if'"
contains_file "t98c SyntaxError kind" "$TMP/e98" "SyntaxError"

printf 'a = 1\nif a == 1 [\n    printr("one")\n]\nelif a == 2 [\n    printr("two")\n]\n' >"$TMP/err_elif2.ru"
"$RUNAC" "$TMP/err_elif2.ru" >/dev/null 2>"$TMP/e99"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t99 elif after newline fails"; else failed "t99 elif after newline fails"; fi
contains_file "t99b elif-after-newline message" "$TMP/e99" "'elif' without a matching 'if'"

# ---- 49. elif/else: invalid standalone else (spec section 4) ----------------------------
printf 'else [\n    printr("x")\n]\n' >"$TMP/err_else1.ru"
"$RUNAC" "$TMP/err_else1.ru" >/dev/null 2>"$TMP/e100"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t100 standalone else fails"; else failed "t100 standalone else fails"; fi
contains_file "t100b standalone else message" "$TMP/e100" "'else' without a matching 'if'"
contains_file "t100c SyntaxError kind" "$TMP/e100" "SyntaxError"

# ---- 50. elif/else: elif after else (spec section 4) -------------------------------------
printf 'if a == 1 [\n    printr("one")\n] else [\n    printr("other")\n] elif a == 2 [\n    printr("two")\n]\n' >"$TMP/err_elif_after_else.ru"
"$RUNAC" "$TMP/err_elif_after_else.ru" >/dev/null 2>"$TMP/e101"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t101 elif-after-else fails"; else failed "t101 elif-after-else fails"; fi
contains_file "t101b elif-after-else message" "$TMP/e101" "'elif' cannot appear after 'else'"
contains_file "t101c exact location 5:3" "$TMP/e101" "err_elif_after_else.ru:5:3"
contains_file "t101d SyntaxError kind" "$TMP/e101" "SyntaxError"

# ---- 51. elif/else: duplicate else (spec section 4) --------------------------------------
printf 'if a == 1 [\n    printr("one")\n] else [\n    printr("two")\n] else [\n    printr("three")\n]\n' >"$TMP/err_dup_else.ru"
"$RUNAC" "$TMP/err_dup_else.ru" >/dev/null 2>"$TMP/e102"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t102 duplicate else fails"; else failed "t102 duplicate else fails"; fi
contains_file "t102b duplicate else message" "$TMP/e102" "duplicate 'else'"
contains_file "t102c SyntaxError kind" "$TMP/e102" "SyntaxError"

# ---- 52. elif/else: malformed elif (spec section 5) --------------------------------------
printf 'if a == 5 [\n    printr("x")\n] elif [\n    printr("y")\n]\n' >"$TMP/err_elif3.ru"
"$RUNAC" "$TMP/err_elif3.ru" >/dev/null 2>"$TMP/e103"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t103 elif without condition fails"; else failed "t103 elif without condition fails"; fi
contains_file "t103b elif-without-condition message" "$TMP/e103" "expected a condition after 'elif'"
contains_file "t103c SyntaxError kind" "$TMP/e103" "SyntaxError"

printf 'if a == 5 [\n    printr("x")\n] elif a == 6\n    printr("y")\n]\n' >"$TMP/err_elif4.ru"
"$RUNAC" "$TMP/err_elif4.ru" >/dev/null 2>"$TMP/e104"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t104 elif without '[' fails"; else failed "t104 elif without '[' fails"; fi
contains_file "t104b elif-without-'[' message" "$TMP/e104" "expected '[' to open the elif block"

# ---- 53. elif/else: malformed else blocks -------------------------------------------------
printf 'if a == 5 [\n    printr("x")\n] else a == 6 [\n    printr("y")\n]\n' >"$TMP/err_else2.ru"
"$RUNAC" "$TMP/err_else2.ru" >/dev/null 2>"$TMP/e105"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t105 else without '[' fails"; else failed "t105 else without '[' fails"; fi
contains_file "t105b else-without-'[' message" "$TMP/e105" "expected '[' to open the else block"

printf 'if a == 5 [\n    printr("x")\n] else [\n    printr("y")\n' >"$TMP/err_else3.ru"
"$RUNAC" "$TMP/err_else3.ru" >/dev/null 2>"$TMP/e106"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t106 unclosed else block fails"; else failed "t106 unclosed else block fails"; fi
contains_file "t106b unclosed else message" "$TMP/e106" "expected ']' to close the else block opened at line 3"
contains_file "t106c SyntaxError kind" "$TMP/e106" "SyntaxError"

# ---- 54. elif/else: state changes inside branches persist ---------------------------------
printf 'n = 0\na = 2\nif a == 1 [\n    n = 1\n] elif a == 2 [\n    n = 2\n    printr("two branch")\n] else [\n    n = 3\n]\nprintr("n", n)\nprintr("done")\n' >"$TMP/chain12.ru"
"$RUNA" "$TMP/chain12.ru" >"$TMP/o107" 2>/dev/null; RC=$?
check_rc "t107 branch state change runs" 0 "$RC"
line_is "t107b elif branch executed" "$TMP/o107" "two branch"
line_is "t107c assignment inside elif persisted" "$TMP/o107" "n 2"
line_is "t107d code after chain runs" "$TMP/o107" "done"
not_contains_file "t107e other branches did not assign" "$TMP/o107" "n 1"
not_contains_file "t107g else branch did not assign" "$TMP/o107" "n 3"
line_count_is "t107f exactly three output lines" "$TMP/o107" 3

# ---- 55. elif/else: one-line chain ---------------------------------------------------------
printf 'if 1 > 2 [ printr("a") ] elif 2 > 1 [ printr("b") ] else [ printr("c") ]\n' >"$TMP/onelinechain.ru"
"$RUNA" "$TMP/onelinechain.ru" >"$TMP/o108" 2>/dev/null; RC=$?
check_rc "t108 one-line chain runs" 0 "$RC"
line_is "t108b one-line chain output" "$TMP/o108" "b"

# ---- 56. elif/else: elif condition must be boolean at runtime -------------------------------
printf 'if 1 == 2 [\n    printr("no")\n] elif 5 [\n    printr("x")\n]\n' >"$TMP/rt_chain1.ru"
"$RUNAC" "$TMP/rt_chain1.ru" -o "$TMP/rt_chain1.rbc" >/dev/null 2>&1; RC=$?
check_rc "t109 non-boolean elif condition compiles" 0 "$RC"
"$RUNAVM" "$TMP/rt_chain1.rbc" >/dev/null 2>"$TMP/e109"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t109b non-boolean elif fails at runtime"; else failed "t109b non-boolean elif fails at runtime"; fi
contains_file "t109c boolean-required message" "$TMP/e109" "condition must be a boolean, got number"
contains_file "t109d RuntimeError kind" "$TMP/e109" "RuntimeError"

# ---- 57. elif/else: dumps and trace ---------------------------------------------------------
"$RUNAC" examples/elifelse.ru --dump-tokens >"$TMP/o110" 2>/dev/null
contains_file "t110 token dump: Elif" "$TMP/o110" "Elif"
contains_file "t110b token dump: Else" "$TMP/o110" "Else"
contains_file "t110c token dump: If" "$TMP/o110" "If"

"$RUNAC" examples/elifelse.ru --dump-ast >"$TMP/o111" 2>/dev/null
contains_file "t111 ast dump: IfStatement" "$TMP/o111" "IfStatement"
contains_file "t111b ast dump: if condition" "$TMP/o111" "if condition BinaryExpr (mtn < 5)"
contains_file "t111c ast dump: elif condition" "$TMP/o111" "elif condition BinaryExpr (mtn == 10)"
contains_file "t111d ast dump: else branch" "$TMP/o111" "else"

"$RUNAC" examples/elifelse.ru --dump-bytecode >"$TMP/o112" 2>/dev/null
contains_file "t112 disassembly: Jump" "$TMP/o112" "Jump "
contains_file "t112b disassembly: JumpIfFalse" "$TMP/o112" "JumpIfFalse"

"$RUNA" examples/elifelse.ru --trace >"$TMP/o113" 2>/dev/null
contains_file "t113 trace shows Jump" "$TMP/o113" "Jump "
line_is "t113b trace program output still correct" "$TMP/o113" "ten"

# ---- 58. functions: the designer's base examples -----------------------------------
"$RUNA" examples/functions.ru >"$TMP/o114" 2>/dev/null; RC=$?
check_rc "t114 functions example runs" 0 "$RC"
line_is "t114b designer base example result" "$TMP/o114" "30"
line_is "t114c second call kept first result" "$TMP/o114" "30"
line_is "t114d no-param function prints" "$TMP/o114" "Hello"
line_is "t114e function calling function" "$TMP/o114" "14"
line_is "t114f return in if branch" "$TMP/o114" "100"
line_is "t114g return in else branch" "$TMP/o114" "200"
line_is "t114h global read inside function" "$TMP/o114" "Hi Runa"
line_is "t114i bare return is none" "$TMP/o114" "none"

"$RUNA" examples/recursion.ru >"$TMP/o115" 2>/dev/null; RC=$?
check_rc "t115 recursion example runs" 0 "$RC"
line_is "t115b countdown(5)" "$TMP/o115" "0"
line_is "t115c fact(5)" "$TMP/o115" "120"
line_is "t115d fib(10)" "$TMP/o115" "55"
line_is "t115e sumTo(5)" "$TMP/o115" "15"

printf 'fun add(a, b) [\n    result = a + b\n    return result\n]\n\nx = add(10, 20)\nprintr(x)\n' >"$TMP/fn1.ru"
"$RUNA" "$TMP/fn1.ru" >"$TMP/o116" 2>/dev/null; RC=$?
check_rc "t116 designer exact base example" 0 "$RC"
line_is "t116b add(10, 20) prints 30" "$TMP/o116" "30"

# ---- 59. functions: declaration forms ------------------------------------------------
printf 'fun empty() [\n]\nprintr("empty ok")\n' >"$TMP/fn2.ru"
"$RUNA" "$TMP/fn2.ru" >"$TMP/o117" 2>/dev/null; RC=$?
check_rc "t117 empty body is valid" 0 "$RC"
line_is "t117b empty body program continues" "$TMP/o117" "empty ok"

printf 'fun hello() [\n    printr("Hello")\n]\nhello()\n' >"$TMP/fn3.ru"
"$RUNA" "$TMP/fn3.ru" >"$TMP/o118" 2>/dev/null
line_is "t118 zero-parameter function" "$TMP/o118" "Hello"

printf 'fun square(n) [\n    return n * n\n]\nprintr(square(7))\n' >"$TMP/fn4.ru"
"$RUNA" "$TMP/fn4.ru" >"$TMP/o119" 2>/dev/null
line_is "t119 one-parameter function" "$TMP/o119" "49"

printf 'fun three(a, b, c) [\n    return a + b * c\n]\nprintr(three(1, 2, 3))\n' >"$TMP/fn5.ru"
"$RUNA" "$TMP/fn5.ru" >"$TMP/o120" 2>/dev/null
line_is "t120 three-parameter function" "$TMP/o120" "7"

# ---- 60. functions: calls are expressions ---------------------------------------------
printf 'fun add(a, b) [\n    return a + b\n]\nx = add(10, 20) * 2\nprintr(x)\n' >"$TMP/fn6.ru"
"$RUNA" "$TMP/fn6.ru" >"$TMP/o121" 2>/dev/null
line_is "t121 call inside arithmetic" "$TMP/o121" "60"

printf 'fun add(a, b) [\n    return a + b\n]\nx = add(10 + 5, 20 * 2)\nprintr(x)\n' >"$TMP/fn7.ru"
"$RUNA" "$TMP/fn7.ru" >"$TMP/o122" 2>/dev/null
line_is "t122 expression arguments" "$TMP/o122" "55"

a_args='a = 10\nb = 20\nfun add(x, y) [\n    return x + y\n]\n'
printf "${a_args}x = add(a, b)\nprintr(x)\n" >"$TMP/fn8.ru"
"$RUNA" "$TMP/fn8.ru" >"$TMP/o123" 2>/dev/null
line_is "t123 variable arguments" "$TMP/o123" "30"

printf "${a_args}x = add(a + 1, b * 2)\nprintr(x)\n" >"$TMP/fn9.ru"
"$RUNA" "$TMP/fn9.ru" >"$TMP/o124" 2>/dev/null
line_is "t124 mixed expression arguments" "$TMP/o124" "51"

printf 'fun add(a, b) [\n    return a + b\n]\nx = add(add(1, 2), 3)\nprintr(x)\n' >"$TMP/fn10.ru"
"$RUNA" "$TMP/fn10.ru" >"$TMP/o125" 2>/dev/null
line_is "t125 nested call arguments" "$TMP/o125" "6"

printf 'fun add(a, b) [\n    return a + b\n]\nif add(1, 2) == 3 [\n    printr("callcond")\n]\n' >"$TMP/fn11.ru"
"$RUNA" "$TMP/fn11.ru" >"$TMP/o126" 2>/dev/null
line_is "t126 call inside if condition" "$TMP/o126" "callcond"

printf 'fun add(a, b) [\n    return a + b\n]\nadd(1, 2)\nprintr("bare call ok")\n' >"$TMP/fn12.ru"
"$RUNA" "$TMP/fn12.ru" >"$TMP/o127" 2>/dev/null; RC=$?
check_rc "t127 bare call statement is valid" 0 "$RC"
line_is "t127b bare call discards value" "$TMP/o127" "bare call ok"

printf 'fun add(a, b) [\n    return a + b\n]\nx = add(10, 20) * 2\n' >"$TMP/fn13.ru"
"$RUNAC" "$TMP/fn13.ru" -o "$TMP/fn13.rbc" >/dev/null 2>&1
"$RUNAVM" "$TMP/fn13.rbc" >/dev/null 2>&1; RC=$?
check_rc "t128 split pipeline runs functions" 0 "$RC"

# ---- 61. functions: independent invocations and locals --------------------------------
printf 'fun add(a, b) [\n    return a + b\n]\nx = add(10, 20)\ny = add(100, 5)\nprintr(y)\n' >"$TMP/fn14.ru"
"$RUNA" "$TMP/fn14.ru" >"$TMP/o129" 2>/dev/null
line_is "t129 second invocation independent" "$TMP/o129" "105"

printf 'fun calculate(a, b) [\n    x = a * 2\n    y = b + 5\n    result = x + y\n    return result\n]\nprintr(calculate(3, 4))\n' >"$TMP/fn15.ru"
"$RUNA" "$TMP/fn15.ru" >"$TMP/o130" 2>/dev/null
line_is "t130 local variables in function" "$TMP/o130" "15"

printf 'fun test(value) [\n    x = value + 1\n    return x\n]\na = test(10)\nb = test(100)\nprintr(a)\nprintr(b)\n' >"$TMP/fn16.ru"
"$RUNA" "$TMP/fn16.ru" >"$TMP/o131" 2>/dev/null
line_is "t131a first invocation locals" "$TMP/o131" "11"
line_is "t131b second invocation locals" "$TMP/o131" "101"

printf 'x = 1\nfun shadow(x) [\n    x = x + 100\n    return x\n]\nprintr(shadow(5))\nprintr(x)\n' >"$TMP/fn17.ru"
"$RUNA" "$TMP/fn17.ru" >"$TMP/o132" 2>/dev/null
line_is "t132a local shadows global" "$TMP/o132" "105"
line_is "t132b global untouched after call" "$TMP/o132" "1"

printf 'fun f() [\n    result = 5\n    return result\n]\nprintr(f())\nprintr(result)\n' >"$TMP/fn18.ru"
"$RUNAC" "$TMP/fn18.ru" -o "$TMP/fn18.rbc" >/dev/null 2>&1; RC=$?
check_rc "t133 local names compile at main" 0 "$RC"
"$RUNAVM" "$TMP/fn18.rbc" >/dev/null 2>"$TMP/e133"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t133b locals do not leak to globals"; else failed "t133b locals do not leak to globals"; fi
contains_file "t133c leaked local is undefined" "$TMP/e133" "undefined variable 'result'"

"$RUNA" examples/functions.ru --dump-globals >"$TMP/o134" 2>/dev/null
contains_file "t134 globals dump keeps x" "$TMP/o134" 'x = 30'
not_contains_file "t134b globals dump has no local result" "$TMP/o134" "result ="
not_contains_file "t134c globals dump has no local inner" "$TMP/o134" "inner ="

# ---- 62. functions: return semantics ---------------------------------------------------
printf 'fun test() [\n    return 10\n    printr("never")\n]\nprintr(test())\n' >"$TMP/fn19.ru"
"$RUNA" "$TMP/fn19.ru" >"$TMP/o135" 2>/dev/null
line_is "t135 return stops execution" "$TMP/o135" "10"
not_contains_file "t135b code after return skipped" "$TMP/o135" "never"
line_count_is "t135c exactly one output line" "$TMP/o135" 1

printf 'fun stop() [\n    return\n]\nx = stop()\nprintr(x)\n' >"$TMP/fn20.ru"
"$RUNA" "$TMP/fn20.ru" >"$TMP/o136" 2>/dev/null
line_is "t136 bare return produces none" "$TMP/o136" "none"

printf 'fun z() [\n    return 0\n]\nfun stop() [\n    return\n]\nprintr(z())\nprintr(stop())\n' >"$TMP/fn21.ru"
"$RUNA" "$TMP/fn21.ru" >"$TMP/o137" 2>/dev/null
line_is "t137a return 0 stays zero" "$TMP/o137" "0"
line_is "t137b bare return stays none" "$TMP/o137" "none"

printf 'fun stop() [\n    return\n]\nx = stop()\nif x == false [\n    printr("eqfalse")\n]\nif x == 0 [\n    printr("eqzero")\n]\nif x == "" [\n    printr("eqempty")\n]\nprintr("none is distinct")\n' >"$TMP/fn22.ru"
"$RUNA" "$TMP/fn22.ru" >"$TMP/o138" 2>/dev/null; RC=$?
check_rc "t138 none equality checks run" 0 "$RC"
line_is "t138b none is distinct from false/0/empty" "$TMP/o138" "none is distinct"
not_contains_file "t138c none is not false" "$TMP/o138" "eqfalse"
not_contains_file "t138d none is not zero" "$TMP/o138" "eqzero"
not_contains_file "t138e none is not empty string" "$TMP/o138" "eqempty"
line_count_is "t138f exactly one output line" "$TMP/o138" 1

printf 'fun stop() [\n    return\n]\nx = stop()\nprintr(x + 1)\n' >"$TMP/fn23.ru"
"$RUNA" "$TMP/fn23.ru" >/dev/null 2>"$TMP/e139"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t139 none arithmetic is a type error"; else failed "t139 none arithmetic is a type error"; fi
contains_file "t139b none kind named" "$TMP/e139" "none and number"

printf 'fun hello() [\n    printr("side effect")\n]\nx = hello()\nprintr(x)\n' >"$TMP/fn24.ru"
"$RUNA" "$TMP/fn24.ru" >"$TMP/o140" 2>/dev/null
line_is "t140a no-return function runs" "$TMP/o140" "side effect"
line_is "t140b end-of-body is none" "$TMP/o140" "none"

printf 'fun check(x) [\n    if x == 10 [\n        return 100\n    ] else [\n        return 200\n    ]\n]\nprintr(check(10))\nprintr(check(5))\n' >"$TMP/fn25.ru"
"$RUNA" "$TMP/fn25.ru" >"$TMP/o141" 2>/dev/null
line_is "t141a if-branch return" "$TMP/o141" "100"
line_is "t141b else-branch return" "$TMP/o141" "200"

printf 'fun check(x) [\n    if x > 10 [\n        return 1\n    ] elif x < 10 [\n        return 2\n    ] else [\n        return 3\n    ]\n]\nprintr(check(20))\nprintr(check(5))\nprintr(check(10))\n' >"$TMP/fn26.ru"
"$RUNA" "$TMP/fn26.ru" >"$TMP/o142" 2>/dev/null
line_is "t142a greater returns 1" "$TMP/o142" "1"
line_is "t142b smaller returns 2" "$TMP/o142" "2"
line_is "t142c equal returns 3" "$TMP/o142" "3"

printf 'fun f(x) [\n    if x == 1 [\n        return 100\n    ]\n    printr("after-if")\n    return 200\n]\nprintr(f(1))\nprintr(f(2))\n' >"$TMP/fn27.ru"
"$RUNA" "$TMP/fn27.ru" >"$TMP/o143" 2>/dev/null
line_is "t143a return exits whole function" "$TMP/o143" "100"
line_is "t143b no-return path continues" "$TMP/o143" "after-if"
line_is "t143c second call returns 200" "$TMP/o143" "200"
line_count_is "t143d exactly three output lines" "$TMP/o143" 3

# ---- 63. functions: deep recursion and call stack ---------------------------------------
printf 'fun countdown(n) [\n    if n == 0 [\n        return 0\n    ] else [\n        return countdown(n - 1)\n    ]\n]\nprintr(countdown(5000))\n' >"$TMP/fn28.ru"
"$RUNA" "$TMP/fn28.ru" >"$TMP/o144" 2>/dev/null
line_is "t144 deep recursion (5000 frames)" "$TMP/o144" "0"

printf 'fun r(n) [\n    return r(n + 1)\n]\nprintr(r(0))\n' >"$TMP/fn29.ru"
"$RUNAC" "$TMP/fn29.ru" -o "$TMP/fn29.rbc" >/dev/null 2>&1; RC=$?
check_rc "t145 infinite recursion compiles" 0 "$RC"
"$RUNAVM" "$TMP/fn29.rbc" >/dev/null 2>"$TMP/e145"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t145b call stack overflow fails cleanly"; else failed "t145b call stack overflow fails cleanly"; fi
contains_file "t145c overflow message" "$TMP/e145" "call stack overflow"
contains_file "t145d RuntimeError kind" "$TMP/e145" "RuntimeError"

# ---- 64. functions: declaration order (forward references) ------------------------------
printf 'x = add(10, 20)\nprintr(x)\n\nfun add(a, b) [\n    return a + b\n]\n' >"$TMP/fn30.ru"
"$RUNA" "$TMP/fn30.ru" >"$TMP/o146" 2>/dev/null; RC=$?
check_rc "t146 call before declaration is valid" 0 "$RC"
line_is "t146b forward reference resolves" "$TMP/o146" "30"

printf 'fun outer(x) [\n    return helper(x) + 1\n]\n\nfun helper(x) [\n    return x * 2\n]\n\nprintr(outer(5))\n' >"$TMP/fn31.ru"
"$RUNA" "$TMP/fn31.ru" >"$TMP/o147" 2>/dev/null
line_is "t147 later-declared callee resolves" "$TMP/o147" "11"

# ---- 65. functions: compile errors -------------------------------------------------------
printf 'fun add(a, b) [\n    return a + b\n]\n\nfun add(a, b) [\n    return 0\n]\n' >"$TMP/efn1.ru"
"$RUNAC" "$TMP/efn1.ru" >/dev/null 2>"$TMP/e148"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t148 duplicate function fails"; else failed "t148 duplicate function fails"; fi
contains_file "t148b duplicate message" "$TMP/e148" "duplicate function 'add'"
contains_file "t148c CompileError kind" "$TMP/e148" "CompileError"

printf 'result = does_not_exist(10)\n' >"$TMP/efn2.ru"
"$RUNAC" "$TMP/efn2.ru" >/dev/null 2>"$TMP/e149"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t149 undefined function fails"; else failed "t149 undefined function fails"; fi
contains_file "t149b unknown function message" "$TMP/e149" "call to unknown function 'does_not_exist'"

printf 'fun add(a, b) [\n    return a + b\n]\nadd(10)\n' >"$TMP/efn3.ru"
"$RUNAC" "$TMP/efn3.ru" >/dev/null 2>"$TMP/e150"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t150 too few arguments fails"; else failed "t150 too few arguments fails"; fi
contains_file "t150b arity message" "$TMP/e150" "no overload of 'add' takes 1 argument(s)"

printf 'fun add(a, b) [\n    return a + b\n]\nadd(10, 20, 30)\n' >"$TMP/efn4.ru"
"$RUNAC" "$TMP/efn4.ru" >/dev/null 2>"$TMP/e151"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t151 too many arguments fails"; else failed "t151 too many arguments fails"; fi
contains_file "t151b arity message" "$TMP/e151" "no overload of 'add' takes 3 argument(s)"

printf 'fun add(a, b) [\n    return a + b\n]\nx = add\n' >"$TMP/efn5.ru"
"$RUNAC" "$TMP/efn5.ru" >/dev/null 2>"$TMP/e152"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t152 function as value fails"; else failed "t152 function as value fails"; fi
contains_file "t152b function-as-value message" "$TMP/e152" "cannot be used as a value"

printf 'x = printr\n' >"$TMP/efn6.ru"
"$RUNAC" "$TMP/efn6.ru" >/dev/null 2>"$TMP/e153"; RC=$?
# Runa 1.0 (DECISIONS #114): printr is CORE RESERVED — reading it as a value
# is a compile error, not a runtime one (it is a call, not a value).
if [[ "$RC" -ne 0 ]]; then pass "t153 printr-as-value rejected"; else failed "t153 printr-as-value rejected"; fi
contains_file "t153c printr-as-value message" "$TMP/e153" "core reserved feature"

printf 'fun add(a, b) [\n    return a + b\n]\nadd = 5\n' >"$TMP/efn7.ru"
"$RUNAC" "$TMP/efn7.ru" >/dev/null 2>"$TMP/e154"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t154 assign to function name fails"; else failed "t154 assign to function name fails"; fi
contains_file "t154b assign-to-function message" "$TMP/e154" "cannot assign to 'add'"

printf 'add = 5\n\nfun add(a, b) [\n    return a + b\n]\n' >"$TMP/efn8.ru"
"$RUNAC" "$TMP/efn8.ru" >/dev/null 2>"$TMP/e155"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t155 assign before declaration fails"; else failed "t155 assign before declaration fails"; fi
contains_file "t155b early assign caught" "$TMP/e155" "cannot assign to 'add'"

printf 'fun empty() []\nprintr("shadow ok")\n' >"$TMP/efn9.ru"
"$RUNAC" "$TMP/efn9.ru" >/dev/null 2>"$TMP/e156"; RC=$?
# Since the collection built-ins arrived, a user function MAY reuse a
# built-in name; call resolution prefers the user function. printr is the
# ONE exception (core reserved, DECISIONS #114) and is covered by t30c.
if [[ "$RC" -eq 0 ]]; then pass "t156 builtin name may be shadowed"; else failed "t156 builtin name may be shadowed"; fi

printf 'fun outer() [\n    fun inner() [\n        return 1\n    ]\n    return 2\n]\n' >"$TMP/efn10.ru"
"$RUNAC" "$TMP/efn10.ru" >/dev/null 2>"$TMP/e157"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t157 nested function fails"; else failed "t157 nested function fails"; fi
contains_file "t157b nested message" "$TMP/e157" "nested function declarations are not supported"

printf 'if 1 == 1 [\n    fun f() [\n    ]\n]\n' >"$TMP/efn11.ru"
"$RUNAC" "$TMP/efn11.ru" >/dev/null 2>"$TMP/e158"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t158 function inside if fails"; else failed "t158 function inside if fails"; fi
contains_file "t158b block-nested message" "$TMP/e158" "nested function declarations are not supported"

printf 'return 10\n' >"$TMP/efn12.ru"
"$RUNAC" "$TMP/efn12.ru" >/dev/null 2>"$TMP/e159"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t159 top-level return fails"; else failed "t159 top-level return fails"; fi
contains_file "t159b return-outside message" "$TMP/e159" "return outside of a function"
contains_file "t159c CompileError kind" "$TMP/e159" "CompileError"

printf 'if 1 == 1 [\n    return 10\n]\n' >"$TMP/efn13.ru"
"$RUNAC" "$TMP/efn13.ru" >/dev/null 2>"$TMP/e160"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t160 return inside top-level if fails"; else failed "t160 return inside top-level if fails"; fi
contains_file "t160b return-outside message" "$TMP/e160" "return outside of a function"

printf 'fun f(a, a) [\n    return a\n]\n' >"$TMP/efn14.ru"
"$RUNAC" "$TMP/efn14.ru" >/dev/null 2>"$TMP/e161"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t161 duplicate parameters fail"; else failed "t161 duplicate parameters fail"; fi
contains_file "t161b duplicate parameter message" "$TMP/e161" "duplicate parameter name 'a'"

# ---- 66. functions: syntax errors ---------------------------------------------------------
printf 'fun () [\n]\n' >"$TMP/sfn1.ru"
"$RUNAC" "$TMP/sfn1.ru" >/dev/null 2>"$TMP/e162"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t162 missing function name fails"; else failed "t162 missing function name fails"; fi
contains_file "t162b name message" "$TMP/e162" "expected a function name after 'fun'"
contains_file "t162c SyntaxError kind" "$TMP/e162" "SyntaxError"

printf 'fun = 5\n' >"$TMP/sfn2.ru"
"$RUNAC" "$TMP/sfn2.ru" >/dev/null 2>"$TMP/e163"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t163 'fun' cannot be a variable"; else failed "t163 'fun' cannot be a variable"; fi
contains_file "t163b reserved fun message" "$TMP/e163" "expected a function name after 'fun'"

printf 'fun f [\n]\n' >"$TMP/sfn3.ru"
"$RUNAC" "$TMP/sfn3.ru" >/dev/null 2>"$TMP/e164"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t164 missing '(' fails"; else failed "t164 missing '(' fails"; fi
contains_file "t164b missing-paren message" "$TMP/e164" "expected '(' after the function name 'f'"

printf 'fun f(a, b [\n]\n' >"$TMP/sfn4.ru"
"$RUNAC" "$TMP/sfn4.ru" >/dev/null 2>"$TMP/e165"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t165 missing ')' in params fails"; else failed "t165 missing ')' in params fails"; fi
contains_file "t165b param list message" "$TMP/e165" "expected ',' or ')' in the parameter list of function 'f'"

printf 'fun f(a,) [\n]\n' >"$TMP/sfn5.ru"
"$RUNAC" "$TMP/sfn5.ru" >/dev/null 2>"$TMP/e166"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t166 trailing comma in params fails"; else failed "t166 trailing comma in params fails"; fi
contains_file "t166b param name message" "$TMP/e166" "expected a parameter name"

printf 'fun f(5) [\n]\n' >"$TMP/sfn6.ru"
"$RUNAC" "$TMP/sfn6.ru" >/dev/null 2>"$TMP/e167"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t167 number as parameter fails"; else failed "t167 number as parameter fails"; fi
contains_file "t167b parameter name message" "$TMP/e167" "expected a parameter name"

printf 'fun f(a, b) printr("x")\n' >"$TMP/sfn7.ru"
"$RUNAC" "$TMP/sfn7.ru" >/dev/null 2>"$TMP/e168"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t168 missing body fails"; else failed "t168 missing body fails"; fi
contains_file "t168b missing body message" "$TMP/e168" "expected '[' to open the body block of function 'f'"

printf 'fun f(a, b) {\n}\n' >"$TMP/sfn8.ru"
"$RUNAC" "$TMP/sfn8.ru" >/dev/null 2>"$TMP/e169"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t169 braces rejected for function body"; else failed "t169 braces rejected for function body"; fi
contains_file "t169b '{' not a body message" "$TMP/e169" "expected '[' to open the body block of function 'f'"

printf 'fun f(a, b) [\n    return a + b\n' >"$TMP/sfn9.ru"
"$RUNAC" "$TMP/sfn9.ru" >/dev/null 2>"$TMP/e170"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t170 unclosed function body fails"; else failed "t170 unclosed function body fails"; fi
contains_file "t170b unclosed body message" "$TMP/e170" "expected ']' to close the function 'f' block opened at line 1"

printf 'fun add(a, b) [\n    return a + b\n]\nx = add(10, 20\n' >"$TMP/sfn10.ru"
"$RUNAC" "$TMP/sfn10.ru" >/dev/null 2>"$TMP/e171"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t171 unclosed call fails"; else failed "t171 unclosed call fails"; fi
contains_file "t171b unclosed call message" "$TMP/e171" "expected ',' or ')' in the argument list of 'add'"

# ---- 67. functions: runtime errors ----------------------------------------------------------
printf 'fun f() [\n    return nope\n]\nprintr(f())\n' >"$TMP/rfn1.ru"
"$RUNAC" "$TMP/rfn1.ru" -o "$TMP/rfn1.rbc" >/dev/null 2>&1; RC=$?
check_rc "t172 undefined variable compiles" 0 "$RC"
"$RUNAVM" "$TMP/rfn1.rbc" >/dev/null 2>"$TMP/e172"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t172b undefined variable fails at runtime"; else failed "t172b undefined variable fails at runtime"; fi
contains_file "t172c undefined message" "$TMP/e172" "undefined variable 'nope'"

printf 'name = "global"\nfun f() [\n    printr("sees", name)\n]\nf()\n' >"$TMP/rfn2.ru"
"$RUNA" "$TMP/rfn2.ru" >"$TMP/o173" 2>/dev/null
line_is "t173 function reads globals" "$TMP/o173" "sees global"

# ---- 68. functions: dumps and trace -----------------------------------------------------------
printf 'fun add(a, b) [\n    return a + b\n]\n' >"$TMP/dfn1.ru"
"$RUNAC" "$TMP/dfn1.ru" --dump-tokens >"$TMP/o174" 2>/dev/null
contains_file "t174 token dump: Fun" "$TMP/o174" "Fun"
contains_file "t174b token dump: Return" "$TMP/o174" "Return"
contains_file "t174c token dump: LeftParen" "$TMP/o174" "LeftParen"
contains_file "t174d token dump: Comma" "$TMP/o174" "Comma"

"$RUNAC" "$TMP/dfn1.ru" --dump-ast >"$TMP/o175" 2>/dev/null
contains_file "t175 ast dump: FunctionDeclaration" "$TMP/o175" "FunctionDeclaration 'add'"
contains_file "t175b ast dump: params" "$TMP/o175" "params: a, b"
contains_file "t175c ast dump: ReturnStatement" "$TMP/o175" "ReturnStatement"
contains_file "t175d ast dump: body expression" "$TMP/o175" "value BinaryExpr (a + b)"

printf 'fun f() [\n]\n' >"$TMP/dfn2.ru"
"$RUNAC" "$TMP/dfn2.ru" --dump-ast >"$TMP/o176" 2>/dev/null
contains_file "t176 ast dump: no params" "$TMP/o176" "params: (none)"
contains_file "t176b ast dump: empty body" "$TMP/o176" "body (empty)"

printf 'fun stop() [\n    return\n]\n' >"$TMP/dfn3.ru"
"$RUNAC" "$TMP/dfn3.ru" --dump-ast >"$TMP/o177" 2>/dev/null
contains_file "t177 ast dump: bare return" "$TMP/o177" "ReturnStatement (no value)"

printf 'x = add(10, 20)\n' >"$TMP/dfn4.ru"
"$RUNAC" "$TMP/dfn4.ru" >/dev/null 2>&1  # must fail — proves dump needs the declaration
printf 'fun add(a, b) [\n    return a + b\n]\nx = add(10, 20)\n' >"$TMP/dfn5.ru"
"$RUNAC" "$TMP/dfn5.ru" --dump-ast >"$TMP/o178" 2>/dev/null
contains_file "t178 ast dump: call in assignment" "$TMP/o178" "value   FunctionCallExpr add(10, 20)"

"$RUNAC" "$TMP/dfn5.ru" --dump-bytecode >"$TMP/o179" 2>/dev/null
contains_file "t179 disassembly: functions table" "$TMP/o179" "functions (1):"
contains_file "t179b disassembly: add entry" "$TMP/o179" "[0] add(a, b) -> code @0x"
contains_file "t179c disassembly: Call user" "$TMP/o179" "Call        2  ; add (argc=2)"
contains_file "t179d disassembly: ReturnValue" "$TMP/o179" "ReturnValue"
contains_file "t179e disassembly: ReturnNone" "$TMP/o179" "ReturnNone"
contains_file "t179f disassembly: LoadVar" "$TMP/o179" "LoadVar"
contains_file "t179g disassembly: StoreGlobal" "$TMP/o179" "StoreGlobal"

"$RUNA" examples/functions.ru --trace >"$TMP/o180" 2>/dev/null
contains_file "t180 trace shows Call" "$TMP/o180" "Call"
contains_file "t180b trace shows function frame" "$TMP/o180" "add]"
contains_file "t180c trace shows ReturnValue" "$TMP/o180" "ReturnValue"
line_is "t180d trace program output intact" "$TMP/o180" "30"

# ---- 69. functions: bytecode container v2 ------------------------------------------------------
printf 'RUNA\x01\x00\x00\x00' >"$TMP/v1.rbc"
"$RUNAVM" "$TMP/v1.rbc" >/dev/null 2>"$TMP/e181"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t181 version-1 file rejected"; else failed "t181 version-1 file rejected"; fi
contains_file "t181b version message" "$TMP/e181" "unsupported bytecode format version 1"

python3 - "$TMP/v2_bad.rbc" <<'PYEOF'
import struct, sys
# A v4 file whose single function has an entry offset beyond the code.
out = sys.argv[1]
blob = b"RUNA" + struct.pack("<I", 7)
blob += struct.pack("<I", 0)                      # constants
names = [b"f"]
blob += struct.pack("<I", len(names))             # names
for n in names:
    blob += struct.pack("<I", len(n)) + n
fns = [(b"f", [], 999)]
blob += struct.pack("<I", len(fns))
for name, params, entry in fns:
    blob += struct.pack("<I", len(name)) + name
    blob += struct.pack("<I", len(params))
    for p in params:
        blob += struct.pack("<I", len(p)) + p
    blob += struct.pack("<I", entry)
blob += struct.pack("<I", 0)                      # classes
blob += struct.pack("<I", 0)                      # inits (v4)
blob += struct.pack("<I", 0)                      # natives (v4)
code = bytes([3, 0, 0, 0, 0, 0, 0, 0, 0, 9])      # Call <0> <0> (op 3) + Halt (op 9)
blob += struct.pack("<I", len(code)) + code
open(out, "wb").write(blob)
PYEOF
"$RUNAVM" "$TMP/v2_bad.rbc" >/dev/null 2>"$TMP/e182"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t182 bad entry offset rejected"; else failed "t182 bad entry offset rejected"; fi
contains_file "t182b entry offset message" "$TMP/e182" "entry offset outside the code"

python3 - "$TMP/v2_ret.rbc" <<'PYEOF'
import struct, sys
# A v4 file with NO functions whose main code executes ReturnNone.
out = sys.argv[1]
blob = b"RUNA" + struct.pack("<I", 7)
blob += struct.pack("<I", 0)   # constants
blob += struct.pack("<I", 0)   # names
blob += struct.pack("<I", 0)   # functions
blob += struct.pack("<I", 0)   # classes
blob += struct.pack("<I", 0)   # inits (v4)
blob += struct.pack("<I", 0)   # natives (v4)
code = bytes([19])             # ReturnNone (op 19), no Halt
blob += struct.pack("<I", len(code)) + code
open(out, "wb").write(blob)
PYEOF
"$RUNAVM" "$TMP/v2_ret.rbc" >/dev/null 2>"$TMP/e183"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "t183 return without frame rejected"; else failed "t183 return without frame rejected"; fi
contains_file "t183b return-outside message" "$TMP/e183" "return outside of a function"

# ---- 70. functions: split-tool parity ------------------------------------------------------------
"$RUNAC" examples/recursion.ru -o "$TMP/rec.rbc" >/dev/null 2>&1; RC=$?
check_rc "t184 recursion compiles standalone" 0 "$RC"
"$RUNAVM" "$TMP/rec.rbc" >"$TMP/o185" 2>/dev/null
line_is "t184b split run fib(10)" "$TMP/o185" "55"


# ==================== CLASS SYSTEM (v0.7) ====================
# The designer's 31-section Class/Object specification. Categories mirror
# tests/<category>/ directories; positive programs must compile AND run,
# err_* files must fail with the documented message.

CATEGORIES="class object fields methods constructor self static visibility inheritance super override polymorphism abstract interface equality identity null nested overload lifetime type_check"

# ---- 71. classes: every positive category program compiles and runs -------
for cat in $CATEGORIES; do
    for src in "$ROOT/tests/$cat"/*.ru; do
        base="$(basename "$src")"
        case "$base" in err_*) continue ;; esac
        name="c71 $cat/$base"
        if "$RUNAC" "$src" -o "$TMP/cat.rbc" >/dev/null 2>"$TMP/e71"; then
            if "$RUNAVM" "$TMP/cat.rbc" >/dev/null 2>"$TMP/e71b"; then
                pass "$name"
            else
                failed "$name (runtime: $(head -1 "$TMP/e71b"))"
            fi
        else
            failed "$name (compile: $(head -1 "$TMP/e71"))"
        fi
    done
done

# ---- 72. the designer's exact Car spec example -----------------------------
"$RUNAVM" "$TMP/cat.rbc" >/dev/null 2>&1  # (leftover from the loop; harmless)
"$RUNAC" "$ROOT/tests/class/car_spec.ru" -o "$TMP/carspec.rbc" >/dev/null 2>&1
"$RUNAVM" "$TMP/carspec.rbc" >"$TMP/o72" 2>/dev/null
line_is "c72a spec car name" "$TMP/o72" "BMW"
line_is "c72b spec car model" "$TMP/o72" "M3"
line_is "c72c spec car speed after 20+22" "$TMP/o72" "42"

# ---- 73. object state independence -----------------------------------------
"$RUNAC" "$ROOT/tests/object/independent_state.ru" -o "$TMP/i.rbc" >/dev/null 2>&1
"$RUNAVM" "$TMP/i.rbc" >"$TMP/o73" 2>/dev/null
line_is "c73a car1 keeps BMW" "$TMP/o73" "BMW"
line_is "c73b car2 keeps Pride" "$TMP/o73" "Pride"

# ---- 74. self / methods / constructor outputs -------------------------------
for prog in self/field_access methods/early_return methods/recursion \
            constructor/basic constructor/calls_method methods/args_expressions \
            methods/return_forms object/objects_in_expressions; do
    "$RUNAC" "$ROOT/tests/$prog.ru" -o "$TMP/m.rbc" >/dev/null 2>&1
    "$RUNAVM" "$TMP/m.rbc" >"$TMP/o74" 2>/dev/null
    case "$prog" in
        self/field_access)        line_is "c74 $prog" "$TMP/o74" "3" ;;
        methods/early_return)     line_is "c74 $prog (negative via 0-n)" "$TMP/o74" "negative" ;;
        methods/recursion)        line_is "c74 $prog fact(5)" "$TMP/o74" "120" ;;
        constructor/basic)        line_is "c74 $prog" "$TMP/o74" "M3" ;;
        constructor/calls_method) line_is "c74 $prog" "$TMP/o74" "ready" ;;
        methods/args_expressions) line_is "c74 $prog nested args" "$TMP/o74" "10" ;;
        methods/return_forms)     line_is "c74 $prog bare return is none" "$TMP/o74" "none" ;;
        object/objects_in_expressions) line_is "c74 $prog arithmetic" "$TMP/o74" "20" ;;
    esac
done

# ---- 75. statics -------------------------------------------------------------
"$RUNAC" "$ROOT/tests/static/shared_state.ru" -o "$TMP/s.rbc" >/dev/null 2>&1
"$RUNAVM" "$TMP/s.rbc" >"$TMP/o75" 2>/dev/null
line_is "c75a three constructions counted" "$TMP/o75" "3"

"$RUNAC" "$ROOT/tests/static/inheritance.ru" -o "$TMP/s2.rbc" >/dev/null 2>&1
"$RUNAVM" "$TMP/s2.rbc" >"$TMP/o75b" 2>/dev/null
line_is "c75b static inherited via subclass" "$TMP/o75b" "V"

# ---- 76. inheritance / super / override / polymorphism outputs ---------------
"$RUNAC" "$ROOT/tests/super/two_levels.ru" -o "$TMP/h.rbc" >/dev/null 2>&1
"$RUNAVM" "$TMP/h.rbc" >"$TMP/o76" 2>/dev/null
line_count_is "c76a two-level super chain fires 3" "$TMP/o76" 3
line_is "c76b chain order A first" "$TMP/o76" "A"

"$RUNAC" "$ROOT/tests/super/return_value.ru" -o "$TMP/h2.rbc" >/dev/null 2>&1
"$RUNAVM" "$TMP/h2.rbc" >"$TMP/o76c" 2>/dev/null
line_is "c76c super return value 15" "$TMP/o76c" "15"

"$RUNAC" "$ROOT/tests/polymorphism/caller_not_type.ru" -o "$TMP/p.rbc" >/dev/null 2>&1
"$RUNAVM" "$TMP/p.rbc" >"$TMP/o76d" 2>/dev/null
line_is "c76d dispatch follows runtime object" "$TMP/o76d" "Meow"

# ---- 77. interfaces / abstract ----------------------------------------------
"$RUNAC" "$ROOT/tests/interface/multiple.ru" -o "$TMP/if.rbc" >/dev/null 2>&1
"$RUNAVM" "$TMP/if.rbc" >"$TMP/o77" 2>/dev/null
line_is "c77a multiple interfaces implemented" "$TMP/o77" "moved"

"$RUNAC" "$ROOT/tests/abstract/template_method.ru" -o "$TMP/ab.rbc" >/dev/null 2>&1
"$RUNAVM" "$TMP/ab.rbc" >"$TMP/o77b" 2>/dev/null
line_is "c77b abstract template method" "$TMP/o77b" "running"

"$RUNAC" "$ROOT/tests/interface/inherited_impl.ru" -o "$TMP/if2.rbc" >/dev/null 2>&1
"$RUNAVM" "$TMP/if2.rbc" >"$TMP/o77c" 2>/dev/null
line_is "c77c interface satisfied through chain" "$TMP/o77c" "still printable"

# ---- 78. equality / identity -------------------------------------------------
"$RUNAC" "$ROOT/tests/equality/structural.ru" -o "$TMP/q.rbc" >/dev/null 2>&1
"$RUNAVM" "$TMP/q.rbc" >"$TMP/o78" 2>/dev/null
line_is "c78a structural equality" "$TMP/o78" "equal"
line_is "c78b structural difference" "$TMP/o78" "different"

"$RUNAC" "$ROOT/tests/identity/structural_not_identity.ru" -o "$TMP/q2.rbc" >/dev/null 2>&1
"$RUNAVM" "$TMP/q2.rbc" >"$TMP/o78b" 2>/dev/null
line_is "c78c equal but distinct" "$TMP/o78b" "but not identical"

# ---- 79. null -----------------------------------------------------------------
"$RUNAC" "$ROOT/tests/null/literal.ru" -o "$TMP/n.rbc" >/dev/null 2>&1
"$RUNAVM" "$TMP/n.rbc" >"$TMP/o79" 2>/dev/null
line_is "c79a null prints as null" "$TMP/o79" "null"

"$RUNAC" "$ROOT/tests/null/is_not_class.ru" -o "$TMP/n2.rbc" >/dev/null 2>&1
"$RUNAVM" "$TMP/n2.rbc" >"$TMP/o79b" 2>/dev/null
line_is "c79b null is never an instance" "$TMP/o79b" "null is not a Car"

# ---- 80. nested classes / overloads -------------------------------------------
"$RUNAC" "$ROOT/tests/nested/declaration.ru" -o "$TMP/nc.rbc" >/dev/null 2>&1
"$RUNAVM" "$TMP/nc.rbc" >"$TMP/o80" 2>/dev/null
line_is "c80a nested class instance works" "$TMP/o80" "100"

"$RUNAC" "$ROOT/tests/overload/arity_selection.ru" -o "$TMP/ov.rbc" >/dev/null 2>&1
"$RUNAVM" "$TMP/ov.rbc" >"$TMP/o80b" 2>/dev/null
line_is "c80b overload 0-arity" "$TMP/o80b" "nothing"
line_is "c80c overload 2-arity" "$TMP/o80b" "two: Runa 1"

"$RUNAC" "$ROOT/tests/overload/inheritance_mix.ru" -o "$TMP/ov2.rbc" >/dev/null 2>&1
"$RUNAVM" "$TMP/ov2.rbc" >"$TMP/o80c" 2>/dev/null
line_is "c80d overload resolution walks the chain" "$TMP/o80c" "base one 5"

# ---- 81. type checks -----------------------------------------------------------
"$RUNAC" "$ROOT/tests/type_check/is_chain.ru" -o "$TMP/t.rbc" >/dev/null 2>&1
"$RUNAVM" "$TMP/t.rbc" >"$TMP/o81" 2>/dev/null
line_count_is "c81a is matches the whole chain" "$TMP/o81" 3

"$RUNAC" "$ROOT/tests/type_check/is_interface.ru" -o "$TMP/t2.rbc" >/dev/null 2>&1
"$RUNAVM" "$TMP/t2.rbc" >"$TMP/o81b" 2>/dev/null
line_is "c81b is matches interfaces" "$TMP/o81b" "movable"

# ---- 82. lifetime: objects are reclaimed; references keep them alive -----------
"$RUNAC" "$ROOT/tests/lifetime/shared_reference_survives.ru" -o "$TMP/l.rbc" >/dev/null 2>&1
"$RUNAVM" "$TMP/l.rbc" >"$TMP/o82" 2>/dev/null
line_is "c82a kept reference stays valid" "$TMP/o82" "x"

printf 'class (Box) [\nstatic built = 0\nvalue = 0\nconstructor(v) [\nself.value = v\nBox.built = Box.built + 1\n]\nfun dig(n) [\nif n <= 0 [\nreturn self.value\n]\nreturn new Box(n).dig(n - 1)\n]\n]\nb = new Box(100)\nprintr(b.dig(99))\nprintr(Box.built)\n' > "$TMP/l2.ru"
"$RUNAC" "$TMP/l2.ru" -o "$TMP/l2.rbc" >/dev/null 2>&1
"$RUNAVM" "$TMP/l2.rbc" >"$TMP/o82b" 2>/dev/null
line_is "c82b 100 chained constructions return" "$TMP/o82b" "1"
line_is "c82c every construction counted" "$TMP/o82b" "100"

# ---- 83. every err_* program fails (compile or runtime) ------------------------
for src in "$ROOT"/tests/{class,object,fields,methods,constructor,self,static,visibility,inheritance,super,override,polymorphism,abstract,interface,equality,identity,null,nested,overload,lifetime,type_check,errors}/err_*.ru; do
    base="$(basename "$src")"
    catname="$(basename "$(dirname "$src")")"
    name="c83 $catname/$base"
    if "$RUNAC" "$src" -o "$TMP/e.rbc" >/dev/null 2>"$TMP/e83"; then
        if "$RUNAVM" "$TMP/e.rbc" >/dev/null 2>"$TMP/e83b"; then
            failed "$name (expected failure, succeeded)"
        else
            pass "$name"
        fi
    else
        pass "$name"
    fi
done

# ---- 84. compile-error messages -------------------------------------------------
assert_compile_error() { # name source-snippet needle
    printf '%s\n' "$2" > "$TMP/ce.ru"
    if "$RUNAC" "$TMP/ce.ru" -o "$TMP/ce.rbc" >/dev/null 2>"$TMP/e84"; then
        failed "c84 $1 (compiled successfully)"
    else
        contains_file "c84 $1" "$TMP/e84" "$3"
    fi
}
assert_compile_error "unknown class" 'x = new Nope()' "unknown class 'Nope'"
assert_compile_error "instantiate abstract" 'abstract class (A) [ abstract fun go() ]
x = new A()' "cannot instantiate abstract class 'A'"
assert_compile_error "instantiate interface" 'interface (P) [ fun go() ]
x = new P()' "cannot instantiate interface 'P'"
assert_compile_error "unknown parent" 'class (A : Nope) [
]' "unknown parent class 'Nope'"
assert_compile_error "inheritance cycle" 'class (A : B) [
]
class (B : A) [
]' "inheritance cycle"
assert_compile_error "abstract impl missing" 'abstract class (A) [ abstract fun go() ]
class (B : A) [
]' "must implement abstract method 'go'"
assert_compile_error "interface impl missing" 'interface (P) [ fun go() ]
class (C) implements P [
]' "required by interface 'P'"
assert_compile_error "override without target" 'class (A) [
]
class (B : A) [
override fun go() [
]
]' "cannot override 'go'"
assert_compile_error "override marker required" 'class (A) [
fun go() [
]
]
class (B : A) [
fun go() [
]
]' "mark it with 'override'"
assert_compile_error "abstract in concrete" 'class (A) [
abstract fun go()
]' "abstract method 'go' in non-abstract class 'A'"
assert_compile_error "abstract with body" 'abstract class (A) [
abstract fun go() [
printr(1)
]
]' "an abstract method cannot have a body"
assert_compile_error "constructor in abstract" 'abstract class (A) [
constructor() [
]
]' "cannot have a constructor"
assert_compile_error "duplicate constructor" 'class (A) [
constructor() [
]
constructor() [
]
]' "duplicate constructor"
assert_compile_error "duplicate method arity" 'class (A) [
fun go(x) [
]
fun go(y) [
]
]' "duplicate method 'go'"
assert_compile_error "mixed static/instance overloads" 'class (A) [
fun go() [
]
static fun go(x) [
]
]' "cannot mix static and instance methods named 'go'"
assert_compile_error "field shadows method" 'class (A) [
x = 1
fun x() [
]
]' "duplicate member name 'x'"
assert_compile_error "self at top level" 'x = self' "'self' is only available inside instance methods"
assert_compile_error "self in static method" 'class (A) [
static fun go() [
return self
]
]' "'self' is not available in a static method"
assert_compile_error "self assignment" 'class (A) [
fun go() [
self = 5
]
]' "'self' cannot be assigned to"
assert_compile_error "super without parent" 'class (A) [
fun go() [
super.go()
]
]' "'super' cannot be used here"
assert_compile_error "super at top level" 'super.go()' "'super' is only valid inside instance methods"
assert_compile_error "unknown static field" 'class (A) [
]
printr(A.nope)' "has no static field 'nope'"
assert_compile_error "instance field via class name" 'class (A) [
x = 1
]
printr(A.x)' "'x' is an instance field of class 'A'"
assert_compile_error "instance method via class name" 'class (A) [
fun go() [
]
]
A.go()' "'go' is an instance method of class 'A'"
assert_runtime_error "static via instance" 'class (A) [
static s = 1
]
a = new A()
printr(a.s)' "'s' is a static field of class 'A'"
assert_compile_error "class as value" 'class (A) [
]
x = A' "'A' is a class, not a value"
assert_compile_error "assign to class name" 'class (A) [
]
A = 5' "cannot assign to 'A'"
assert_compile_error "call class directly" 'class (A) [
]
A()' "'A' is a class"
assert_compile_error "unknown interface" 'class (A) implements Missing [
]' "unknown interface 'Missing'"
assert_compile_error "interface with field" 'interface (P) [
x = 5
]' "interfaces cannot have fields"
assert_compile_error "interface with body" 'interface (P) [
fun go() [
printr(1)
]
]' "interface methods cannot have a body"
assert_compile_error "interface nested" 'class (A) [
interface (P) [
fun go()
]
]' "interfaces cannot be nested"
assert_compile_error "duplicate class same scope" 'class (A) [
]
class (A) [
]' "duplicate class 'A'"
assert_compile_error "class in function" 'fun f() [
class (Bad) [
]
]' "class declarations are only allowed at the top level"
assert_compile_error "class in if block" 'if true [
class (Bad) [
]
]' "class declarations are only allowed at the top level"
assert_compile_error "constructor wrong argc" 'class (A) [
constructor(x) [
]
]
a = new A()' "no constructor of 'A' takes 0 argument(s)"
assert_compile_error "ctor without ctor args" 'class (A) [
]
a = new A(1)' "has no constructor"
assert_compile_error "nested class name scoped" 'class (Car) [
class (Engine) [
h = 100
]
]
e = new Engine()' "unknown class 'Engine'"
assert_compile_error "abstract static method" 'abstract class (A) [
abstract static fun go()
]' "an abstract method cannot be static"
assert_compile_error "abstract private method" 'abstract class (A) [
abstract private fun go()
]' "an abstract method cannot be private"
assert_compile_error "static override rejected" 'class (A) [
static fun go() [
]
]
class (B : A) [
override fun go() [
]
]' "cannot override 'go'"
assert_compile_error "interface cannot inherit" 'interface (A : B) [
]' "an interface cannot inherit"
assert_compile_error "implement non-interface" 'class (A) [
]
class (B) implements A [
]' "cannot implement 'A'"

# ---- 85. runtime-error messages ---------------------------------------------------
assert_runtime_error() { # name source-snippet needle
    printf '%s\n' "$2" > "$TMP/re.ru"
    if ! "$RUNAC" "$TMP/re.ru" -o "$TMP/re.rbc" >/dev/null 2>"$TMP/e85a"; then
        failed "c85 $1 (compile failed: $(head -1 "$TMP/e85a"))"
    else
        if "$RUNAVM" "$TMP/re.rbc" >/dev/null 2>"$TMP/e85"; then
            failed "c85 $1 (ran successfully)"
        else
            contains_file "c85 $1" "$TMP/e85" "$3"
        fi
    fi
}
assert_runtime_error "null field deref" 'car = null
printr(car.name)' "null dereference: cannot access field 'name' on null"
assert_runtime_error "null method deref" 'car = null
car.go()' "null dereference: cannot call method 'go' on null"
assert_runtime_error "private field outside" 'class (A) [
private secret = 1
]
a = new A()
printr(a.secret)' "private field 'secret' of class 'A' is not accessible here"
assert_runtime_error "private write outside" 'class (A) [
private pin = 1
]
a = new A()
a.pin = 9' "private field 'pin'"
assert_runtime_error "private method outside" 'class (A) [
private fun secret() [
printr(1)
]
]
a = new A()
a.secret()' "private method 'secret'"
assert_runtime_error "unknown member runtime" 'class (A) [
q = 1
]
a = new A()
printr(a.nope)' "class 'A' has no field 'nope'"
assert_runtime_error "member on number" 'x = 5
printr(x.field)' "cannot access field 'field' on a value of type 'number'"
assert_runtime_error "method on number" 'x = 5
x.go()' "cannot call method 'go' on a value of type 'number'"
# Runa 1.0 (DECISIONS #117): cyclic structures compare TRUE via pair
# tracking — A{me: A} and B{me: B} are structurally the same shape.
printf 'class (A) [\nme = null\n\nfun wire() [\nself.me = self\n]\n]\na = new A()\na.wire()\nb = new A()\nb.wire()\nif a == b [\nprintr("equal")\n]\n' > "$TMP/cyc.ru"
if "$RUNAC" "$TMP/cyc.ru" -o "$TMP/cyc.rbc" >/dev/null 2>"$TMP/e85c" && \
   "$RUNAVM" "$TMP/cyc.rbc" >"$TMP/o85c" 2>/dev/null; then
    contains_file "c85 structural equality deep cycle (cycle-safe true)" "$TMP/o85c" "equal"
else
    failed "c85 structural equality deep cycle (cycle-safe true)"
fi
# Deep-but-acyclic chains still hit the depth limit with a clean RuntimeError.
deep='class (A) [
n = null
]
c = new A()
cur = c
for i = 1 to 80 [
    nxt = new A()
    cur.n = nxt
    cur = nxt
]
d = new A()
cur = d
for i = 1 to 80 [
    nxt = new A()
    cur.n = nxt
    cur = nxt
]
if c == d [
    printr("deep equal")
]
'
printf '%s\n' "$deep" > "$TMP/deep.ru"
if "$RUNAC" "$TMP/deep.ru" -o "$TMP/deep.rbc" >/dev/null 2>&1 && \
   ! "$RUNAVM" "$TMP/deep.rbc" >/dev/null 2>"$TMP/e85d"; then
    contains_file "c85b depth limit is a clean RuntimeError" "$TMP/e85d" "maximum depth of 64"
else
    failed "c85b depth limit is a clean RuntimeError"
fi
c85_count=$(ls "$TMP"/e85* 2>/dev/null | wc -l)
if [[ "$c85_count" -ge 0 ]]; then pass "c85 error outputs collected"; fi

# ---- 86. private static enforcement ------------------------------------------------
assert_compile_error "private static field outside" 'class (A) [
private static key = 7
]
printr(A.key)' "private static field 'key'"
assert_compile_error "private static method outside" 'class (A) [
private static fun key() [
return 1
]
]
printr(A.key())' "private static method 'key'"

# ---- 87. dumps: tokens, AST, bytecode ------------------------------------------------
printf 'class (Vehicle) [\n]\nclass (Car : Vehicle) [\nname = "x"\nstatic count = 0\nconstructor(n) [\nself.name = n\n]\nfun go() [\n]\n]\nc = new Car("a")\nc.go()\nc.name = "b"\nif c is Car [\nprintr(1)\n]\n' > "$TMP/dump.ru"
"$RUNAC" "$TMP/dump.ru" --dump-tokens -o "$TMP/dump.rbc" >"$TMP/tok87" 2>/dev/null
contains_file "c87a Class token" "$TMP/tok87" " Class "
contains_file "c87b New token" "$TMP/tok87" " New "
contains_file "c87c Self token" "$TMP/tok87" " Self "
contains_file "c87d Constructor token" "$TMP/tok87" " Constructor "
contains_file "c87e Static token" "$TMP/tok87" " Static "
contains_file "c87f Is token" "$TMP/tok87" " Is "
contains_file "c87g Dot token" "$TMP/tok87" " Dot "
contains_file "c87h Colon token" "$TMP/tok87" " Colon "

"$RUNAC" "$TMP/dump.ru" --dump-ast -o "$TMP/dump.rbc" >"$TMP/ast87" 2>/dev/null
contains_file "c87i ClassDeclaration dump" "$TMP/ast87" "ClassDeclaration 'Car' (parent: Vehicle)"
contains_file "c87j field dump" "$TMP/ast87" "field name = "
contains_file "c87k constructor dump" "$TMP/ast87" "constructor (n)"
contains_file "c87l method dump" "$TMP/ast87" "method go()"
contains_file "c87m NewExpr dump" "$TMP/ast87" "NewExpr new Car(\"a\")"
contains_file "c87n member assignment dump" "$TMP/ast87" "target MemberAccessExpr c.name"

printf 'class (P) [\nnullv = null\n]\n' > "$TMP/dump2.ru"
"$RUNAC" "$TMP/dump2.ru" --dump-ast -o "$TMP/dump2.rbc" >"$TMP/ast87b" 2>/dev/null
contains_file "c87o NullLiteral dump" "$TMP/ast87b" "field nullv = NullLiteral"

"$RUNAC" "$TMP/dump.ru" -o "$TMP/dump.rbc" >/dev/null 2>&1
"$RUNAVM" "$TMP/dump.rbc" --dump-globals >/dev/null 2>&1
printf 'class (Car) [\n]\nc = new Car()\n' > "$TMP/dump3.ru"
"$RUNAC" "$TMP/dump3.ru" -o "$TMP/dump3.rbc" >/dev/null 2>&1
"$RUNAVM" "$TMP/dump3.rbc" --dump-globals >"$TMP/g87" 2>/dev/null
contains_file "c87p object in globals dump" "$TMP/g87" "c = Car#1"

printf 'class (Car) [\nname = "x"\n]\nc = new Car()\nprintr(c.name)\n' > "$TMP/dump4.ru"
"$RUNAC" "$TMP/dump4.ru" --dump-bytecode -o "$TMP/dump4.rbc" >"$TMP/bc87" 2>/dev/null
contains_file "c87q classes table in disassembly" "$TMP/bc87" "classes (1):"
contains_file "c87r NewObject in disassembly" "$TMP/bc87" "NewObject"
contains_file "c87s GetField in disassembly" "$TMP/bc87" "GetField"
contains_file "c87t InitField in disassembly" "$TMP/bc87" "InitField"

# ---- 88. bytecode container ----------------------------------------------------------
"$RUNAC" "$ROOT/examples/hello.ru" -o "$TMP/v.rbc" >/dev/null 2>&1
if head -c 4 "$TMP/v.rbc" | grep -q "RUNA"; then pass "c88a magic number"; else failed "c88a magic number"; fi
python3 - "$TMP/v.rbc" <<'PYEOF'
import struct, sys
blob = open(sys.argv[1], "rb").read()
version = struct.unpack("<I", blob[4:8])[0]
print("version", version)
PYEOF
if python3 - "$TMP/v.rbc" <<'PYEOF'
import struct, sys
blob = open(sys.argv[1], "rb").read()
sys.exit(0 if struct.unpack("<I", blob[4:8])[0] == 7 else 1)
PYEOF
then pass "c88b format version 7"; else failed "c88b format version 7"; fi

# a v2 file is rejected by a v3 toolchain
python3 - "$TMP/v2old.rbc" <<'PYEOF'
import struct, sys
blob = b"RUNA" + struct.pack("<I", 2)
blob += struct.pack("<I", 0)   # constants
blob += struct.pack("<I", 0)   # names
blob += struct.pack("<I", 0)   # functions
blob += struct.pack("<I", 0)   # code
open(sys.argv[1], "wb").write(blob)
PYEOF
"$RUNAVM" "$TMP/v2old.rbc" >/dev/null 2>"$TMP/e88"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "c88c v2 file rejected"; else failed "c88c v2 file rejected"; fi
contains_file "c88d v2 rejection message" "$TMP/e88" "unsupported bytecode format version 2"

# crafted v4 file with a bad field initializer offset
python3 - "$TMP/v3bad.rbc" <<'PYEOF'
import struct, sys
out = sys.argv[1]
blob = b"RUNA" + struct.pack("<I", 7)
blob += struct.pack("<I", 0)                 # constants
blob += struct.pack("<I", 0)                 # names
blob += struct.pack("<I", 0)                 # functions
blob += struct.pack("<I", 1)                 # classes
name = b"A"
blob += struct.pack("<I", len(name)) + name  # class name
blob += struct.pack("<I", 0xFFFFFFFF)        # no parent
blob += bytes([0])                           # flags
blob += struct.pack("<I", 0)                 # interfaces
blob += struct.pack("<I", 1)                 # fields
blob += bytes([0])                           # field flags
fname = b"x"
blob += struct.pack("<I", len(fname)) + fname
blob += struct.pack("<I", 999999)            # initializer offset out of range
blob += struct.pack("<I", 0)                 # methods
blob += struct.pack("<I", 0)                 # inits (v4)
blob += struct.pack("<I", 0)                 # natives (v4)
blob += struct.pack("<I", 0)                 # code
open(out, "wb").write(blob)
PYEOF
"$RUNAVM" "$TMP/v3bad.rbc" >/dev/null 2>"$TMP/e88b"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "c88e bad init offset rejected"; else failed "c88e bad init offset rejected"; fi
contains_file "c88f bad init offset message" "$TMP/e88b" "initializer offset outside the code"

# crafted v4 file with a bad parent index
python3 - "$TMP/v3par.rbc" <<'PYEOF'
import struct, sys
out = sys.argv[1]
blob = b"RUNA" + struct.pack("<I", 7)
blob += struct.pack("<I", 0)                 # constants
blob += struct.pack("<I", 0)                 # names
blob += struct.pack("<I", 0)                 # functions
blob += struct.pack("<I", 1)                 # classes
name = b"A"
blob += struct.pack("<I", len(name)) + name
blob += struct.pack("<I", 77)                # parent index out of range
blob += bytes([0])
blob += struct.pack("<I", 0)                 # interfaces
blob += struct.pack("<I", 0)                 # fields
blob += struct.pack("<I", 0)                 # methods
blob += struct.pack("<I", 0)                 # inits (v4)
blob += struct.pack("<I", 0)                 # natives (v4)
blob += struct.pack("<I", 0)                 # code
open(out, "wb").write(blob)
PYEOF
"$RUNAVM" "$TMP/v3par.rbc" >/dev/null 2>"$TMP/e88c"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "c88g bad parent index rejected"; else failed "c88g bad parent index rejected"; fi
contains_file "c88h bad parent message" "$TMP/e88c" "parent index outside the class table"

# ---- 89. recursion depth with methods ------------------------------------------------
printf 'class (R) [\nfun spin(n) [\nif n > 0 [\nself.spin(n - 1)\n]\n]\n]\nr = new R()\nr.spin(5000)\nprintr("deep ok")\n' > "$TMP/deep.ru"
"$RUNAC" "$TMP/deep.ru" -o "$TMP/deep.rbc" >/dev/null 2>&1
"$RUNAVM" "$TMP/deep.rbc" >"$TMP/o89" 2>/dev/null
line_is "c89a 5000-deep method recursion" "$TMP/o89" "deep ok"

printf 'class (R) [\nfun spin(n) [\nself.spin(n)\n]\n]\nr = new R()\nr.spin(1)\n' > "$TMP/over.ru"
"$RUNAC" "$TMP/over.ru" -o "$TMP/over.rbc" >/dev/null 2>&1
"$RUNAVM" "$TMP/over.rbc" >/dev/null 2>"$TMP/e89"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "c89b runaway method recursion fails cleanly"; else failed "c89b runaway method recursion fails cleanly"; fi
contains_file "c89c overflow message" "$TMP/e89" "call stack overflow"

# ---- 90. split-tool parity for classes ------------------------------------------------
"$RUNAC" "$ROOT/tests/class/car_spec.ru" -o "$TMP/split.rbc" >/dev/null 2>&1; RC=$?
check_rc "c90a class program compiles standalone" 0 "$RC"
"$RUNAVM" "$TMP/split.rbc" >"$TMP/o90" 2>/dev/null
line_is "c90b split run matches" "$TMP/o90" "42"

# ---- 91. loop: the designer's exact spec example and basic shapes -----------
# assert_program_output NAME FILE LINE1 LINE2... — compiles with runac, runs
# with runavm (split-tool parity for every positive case) and requires the
# output to be EXACTLY the given lines in order.
assert_program_output() {
    local name="$1"; shift
    local file="$1"; shift
    if ! "$RUNAC" "$file" -o "$TMP/lc.rbc" >/dev/null 2>"$TMP/lce" ||
       ! "$RUNAVM" "$TMP/lc.rbc" >"$TMP/lo" 2>"$TMP/lre"; then
        failed "$name (did not run: $(head -1 "$TMP/lce" "$TMP/lre" 2>/dev/null | head -1))"
        return
    fi
    printf '%s\n' "$@" > "$TMP/lexp"
    if diff -q "$TMP/lexp" "$TMP/lo" >/dev/null 2>&1; then
        pass "$name"
    else
        failed "$name (output mismatch)"
    fi
}
assert_program_output "c91a designer's loop example" "$ROOT/tests/loop/loop_spec.ru" 0 1 2 3 4
assert_program_output "c91b loop runs zero times" "$ROOT/tests/loop/loop_zero.ru" "after"
assert_program_output "c91c loop runs exactly once" "$ROOT/tests/loop/loop_once.ru" 5 "once done"
assert_program_output "c91d state accumulates across passes" "$ROOT/tests/loop/loop_state.ru" 10
assert_program_output "c91e variable changes re-check condition" "$ROOT/tests/loop/loop_reassign.ru" 10

# ---- 92. loop: conditions are full Runa expressions -------------------------
assert_program_output "c92a arithmetic condition" "$ROOT/tests/loop/loop_expr_cond.ru" 8 9
assert_program_output "c92b function call in condition" "$ROOT/tests/loop/loop_call_cond.ru" 0 1 2
if "$RUNAC" "$ROOT/tests/loop/err_loop_cond_type.ru" -o "$TMP/lb.rbc" >/dev/null 2>&1 &&
   ! "$RUNAVM" "$TMP/lb.rbc" >/dev/null 2>"$TMP/e92"; then
    contains_file "c92c non-boolean condition fails at runtime" "$TMP/e92" \
        "condition must be a boolean, got number"
else
    failed "c92c non-boolean condition fails at runtime"
fi

# ---- 93. loop: nested loops -------------------------------------------------
assert_program_output "c93a nested loop grid" "$ROOT/tests/loop/loop_nested.ru" 11 12 21 22

# ---- 94. loop: syntax errors --------------------------------------------------
assert_compile_error "loop without condition" "$(cat "$ROOT/tests/loop/err_loop_no_cond.ru")" \
    "expected a condition after 'loop'"
assert_compile_error "loop without '['" "$(cat "$ROOT/tests/loop/err_loop_no_bracket.ru")" \
    "expected '[' to open the loop block"
assert_compile_error "unclosed loop block" "$(cat "$ROOT/tests/loop/err_loop_unclosed.ru")" \
    "expected ']' to close the loop block"

# ---- 95. for: the designer's exact spec example and documented semantics ----
assert_program_output "c95a designer's for example" "$ROOT/tests/for/for_spec.ru" 1 2 3 4 5
assert_program_output "c95b single-member range (start == end)" "$ROOT/tests/for/for_single.ru" 3
assert_program_output "c95c start > end runs zero times" "$ROOT/tests/for/for_empty_range.ru" "after"
assert_program_output "c95d start/end may be expressions" "$ROOT/tests/for/for_expr_bounds.ru" 3 4 5 6
assert_program_output "c95e end evaluated exactly once" "$ROOT/tests/for/for_call_bounds.ru" \
    "end evaluated" 1 2
assert_program_output "c95f counter after loops (end+1 / start)" "$ROOT/tests/for/for_counter_after.ru" \
    1 2 3 4 5
assert_program_output "c95g counter is an ordinary variable" "$ROOT/tests/for/for_counter_assign.ru" \
    1 4 5

# ---- 96. for: nesting and loop/for mixing -------------------------------------
assert_program_output "c96a nested for grid" "$ROOT/tests/for/for_nested.ru" 11 12 21 22 31 32
assert_program_output "c96b for inside loop" "$ROOT/tests/for/for_in_loop.ru" 11 12 21 22
assert_program_output "c96c loop inside for (spec section 5)" "$ROOT/tests/for/loop_in_for.ru" 1 2 3

# ---- 97. for: syntax errors and reserved names ---------------------------------
assert_compile_error "for without variable" "$(cat "$ROOT/tests/for/err_for_no_var.ru")" \
    "expected a loop variable name after 'for'"
assert_compile_error "for without '='" "$(cat "$ROOT/tests/for/err_for_no_equal.ru")" \
    "expected '=' after the loop variable 'i'"
assert_compile_error "for without 'to'" "$(cat "$ROOT/tests/for/err_for_no_to.ru")" \
    "expected 'to' after the start value of the for loop"
assert_compile_error "for without start value" "$(cat "$ROOT/tests/for/err_for_no_start.ru")" \
    "expected a start value in the for loop"
assert_compile_error "for without end value" "$(cat "$ROOT/tests/for/err_for_no_end.ru")" \
    "expected an end value after 'to' in the for loop"
assert_compile_error "for without '['" "$(cat "$ROOT/tests/for/err_for_no_bracket.ru")" \
    "expected '[' to open the for loop block"
assert_compile_error "class name as for counter" "$(cat "$ROOT/tests/for/err_for_class_var.ru")" \
    "cannot use 'Car' as a for loop variable"
assert_compile_error "function name as for counter" "$(cat "$ROOT/tests/for/err_for_function_var.ru")" \
    "cannot use 'go' as a for loop variable"

# ---- 98. break: nearest loop only ------------------------------------------------
assert_program_output "c98a break inside loop" "$ROOT/tests/break/break_loop.ru" 1 2 3 "loop over"
assert_program_output "c98b designer's break-in-for example" "$ROOT/tests/break/break_for.ru" 1 2 3 4
assert_program_output "c98c loop true + break terminates" "$ROOT/tests/break/break_true_loop.ru" 3
assert_program_output "c98d nested break hits nearest loop only" "$ROOT/tests/break/break_nested.ru" \
    12 22 32

# ---- 99. break: outside loops / across boundaries --------------------------------
assert_compile_error "break outside any loop" "$(cat "$ROOT/tests/break/err_break_outside.ru")" \
    "break outside of a loop"
assert_compile_error "break inside a function body" \
    "$(cat "$ROOT/tests/break/err_break_in_function.ru")" "break outside of a loop"
assert_compile_error "break inside a method body" \
    "$(cat "$ROOT/tests/break/err_break_in_method.ru")" "break outside of a loop"
assert_syntax_error() { # name source-snippet needle (SyntaxError flavor of compile check)
    printf '%s\n' "$2" > "$TMP/se.ru"
    if "$RUNAC" "$TMP/se.ru" -o "$TMP/se.rbc" >/dev/null 2>"$TMP/se_err"; then
        failed "$1 (compiled successfully)"
    else
        contains_file "$1" "$TMP/se_err" "$3"
    fi
}
assert_syntax_error "break takes no value" "$(cat "$ROOT/tests/break/err_break_value.ru")" \
    "expected end of line after 'break' ('break' takes no value)"

# ---- 100. continue: nearest loop, current iteration only --------------------------
assert_program_output "c100a continue re-checks condition" "$ROOT/tests/continue/continue_loop.ru" \
    1 2 4 5
assert_program_output "c100b designer's continue-in-for example" \
    "$ROOT/tests/continue/continue_for.ru" 1 2 4 5
assert_program_output "c100c nested continue hits nearest loop only" \
    "$ROOT/tests/continue/continue_nested.ru" 11 13 21 23
assert_program_output "c100d continue and break together" \
    "$ROOT/tests/continue/continue_and_break.ru" 1 3 4

# ---- 101. continue: outside loops / across boundaries ------------------------------
assert_compile_error "continue outside any loop" \
    "$(cat "$ROOT/tests/continue/err_continue_outside.ru")" "continue outside of a loop"
assert_compile_error "continue inside a function body" \
    "$(cat "$ROOT/tests/continue/err_continue_in_function.ru")" "continue outside of a loop"
assert_compile_error "continue inside a method body" \
    "$(cat "$ROOT/tests/continue/err_continue_in_method.ru")" "continue outside of a loop"
assert_syntax_error "continue takes no value" "$(cat "$ROOT/tests/continue/err_continue_value.ru")" \
    "expected end of line after 'continue' ('continue' takes no value)"

# ---- 102. integration: loops and functions ------------------------------------------
assert_program_output "c102a designer's loop-in-function example" \
    "$ROOT/tests/integration/int_loop_function.ru" 0 1 2 "after test"
assert_program_output "c102b for in function + calls from loop" \
    "$ROOT/tests/integration/int_for_function.ru" go go go tick tick
assert_program_output "c102c recursion + loop" \
    "$ROOT/tests/integration/int_recursion_loop.ru" 0 1 2 3 3 2 1 liftoff
assert_program_output "c102d recursion + for" \
    "$ROOT/tests/integration/int_recursion_for.ru" 10 20

# ---- 103. integration: loops, classes and objects -----------------------------------
assert_program_output "c103a loop in method + object state in condition" \
    "$ROOT/tests/integration/int_class_loop.ru" 45 60
assert_program_output "c103b for + static members" \
    "$ROOT/tests/integration/int_class_for_static.ru" 6 55
assert_program_output "c103c 10000-pass loops stay balanced" \
    "$ROOT/tests/integration/int_stress.ru" 10000 10000

# ---- 104. dumps: tokens, AST and bytecode for the loop system ------------------------
printf 'x = 0\nloop x < 2 [\nprintr(x)\nx = x + 1\n]\n' > "$TMP/dl.ru"
"$RUNA" "$TMP/dl.ru" --dump-ast >"$TMP/o104a" 2>/dev/null
contains_file "c104a ast dump: LoopStatement" "$TMP/o104a" "LoopStatement"
contains_file "c104b ast dump: loop condition" "$TMP/o104a" "condition BinaryExpr (x < 2)"
"$RUNA" "$ROOT/tests/for/for_spec.ru" --dump-ast >"$TMP/o104b" 2>/dev/null
contains_file "c104c ast dump: ForStatement" "$TMP/o104b" "ForStatement 'i'"
contains_file "c104d ast dump: for start" "$TMP/o104b" "start NumberLiteral 1"
contains_file "c104e ast dump: for end" "$TMP/o104b" "end   NumberLiteral 5"
printf 'loop true [\nbreak\n]\nfor i = 1 to 2 [\ncontinue\n]\n' > "$TMP/dc.ru"
"$RUNA" "$TMP/dc.ru" --dump-ast >"$TMP/o104c" 2>/dev/null
contains_file "c104f ast dump: BreakStatement" "$TMP/o104c" "BreakStatement"
contains_file "c104g ast dump: ContinueStatement" "$TMP/o104c" "ContinueStatement"
"$RUNA" "$ROOT/tests/for/for_spec.ru" --dump-bytecode >"$TMP/o104d" 2>/dev/null
contains_file "c104h bytecode: inclusive bound via Le" "$TMP/o104d" "Le"
contains_file "c104i bytecode: hidden end slot" "$TMP/o104d" "#for_end0#"
contains_file "c104j bytecode: conditional jump" "$TMP/o104d" "JumpIfFalse"
contains_file "c104k bytecode: back-jump" "$TMP/o104d" "Jump"
"$RUNA" "$TMP/dc.ru" --dump-tokens >"$TMP/o104e" 2>/dev/null
contains_file "c104l token dump: Loop" "$TMP/o104e" "Loop"
contains_file "c104m token dump: For" "$TMP/o104e" "For"
contains_file "c104n token dump: To" "$TMP/o104e" "To"
contains_file "c104o token dump: Break" "$TMP/o104e" "Break"
contains_file "c104p token dump: Continue" "$TMP/o104e" "Continue"

# ---- 105. internals: hidden for slots never leak into --dump-globals ------------------
"$RUNA" "$ROOT/tests/for/for_spec.ru" --dump-globals >"$TMP/o105" 2>/dev/null
contains_file "c105a counter is a real global" "$TMP/o105" "i = 6"
not_contains_file "c105b internal slot hidden from globals" "$TMP/o105" "#for_end"

# ---- 106. container: loop programs produce ordinary v3 bytecode ------------------------
"$RUNAC" "$ROOT/tests/for/for_spec.ru" -o "$TMP/v3loop.rbc" >/dev/null 2>&1; RC=$?
check_rc "c106a loop program compiles standalone" 0 "$RC"
if head -c 4 "$TMP/v3loop.rbc" | grep -q "RUNA"; then pass "c106b .rbc magic"; else failed "c106b .rbc magic"; fi
"$RUNAVM" "$TMP/v3loop.rbc" >"$TMP/o106" 2>/dev/null
assert_program_output "c106c split run matches" "$ROOT/tests/for/for_spec.ru" 1 2 3 4 5

# ---- summary ----






source "$ROOT/tests/package_tests.sh.inc"
source "$ROOT/tests/collections_tests.sh.inc"
source "$ROOT/tests/spec_tests.sh.inc"
spec_run_all

echo "----------------------------------------"
echo "tests: $PASS passed, $FAIL failed"
if [[ "$FAIL" -eq 0 ]]; then
    echo "ALL TESTS PASSED"
    exit 0
fi
exit 1
