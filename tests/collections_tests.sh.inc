
# ################################################################################
# # v1.0-rc — COLLECTION SYSTEM: {..} literals, set semantics, built-ins         #
# ################################################################################
# Fixtures live under tests/collections/ (one directory per spec section);
# every layer is driven through the real tools (runac/runavm).

COL="$ROOT/tests/collections"

# Compile fails cleanly with the expected message.
col_compile_fail() { # name file needle
    if "$RUNAC" "$2" -o "$TMP/col.rbc" >/dev/null 2>"$TMP/col.err"; then
        failed "$1 (compiled, expected error: $3)"
    else
        contains_file "$1" "$TMP/col.err" "$3"
    fi
}

# Compiles, then the VM fails cleanly with the expected message (no crash).
col_runtime_fail() { # name file needle
    if "$RUNAC" "$2" -o "$TMP/col.rbc" >/dev/null 2>"$TMP/col.ce" &&
       ! "$RUNAVM" "$TMP/col.rbc" >/dev/null 2>"$TMP/col.err"; then
        contains_file "$1" "$TMP/col.err" "$3"
    else
        failed "$1 (expected a clean runtime error: $3)"
    fi
}

# ---- 1. literals -------------------------------------------------------------
assert_program_output "col literal basic" "$COL/literals/lit_basic.ru" "{10, 20, 30, 40}"
assert_program_output "col literal strings" "$COL/literals/lit_strings.ru" '{"Lumo", "Runa", "Tluxe"}'
assert_program_output "col literal booleans" "$COL/literals/lit_booleans.ru" "{false, true}"
assert_program_output "col literal variables" "$COL/literals/lit_variables.ru" "{10, 20}"
assert_program_output "col literal expressions" "$COL/literals/lit_expressions.ru" "{10, 20, 30}"
assert_program_output "col literal inside call" "$COL/literals/lit_in_call.ru" "{5, 6, 7}" 3
assert_program_output "col literal single element" "$COL/literals/lit_single.ru" "{10}" 1
assert_program_output "col literal as field value" "$COL/literals/lit_member_assign.ru" "{1, 2}" "{7, 8, 9}"

# ---- 2. empty ----------------------------------------------------------------
assert_program_output "col empty literal prints braces" "$COL/empty/empty_literal.ru" "{}" 0
assert_program_output "col empty builtin" "$COL/empty/empty_builtin.ru" "true" "false"
assert_program_output "col empty is not null" "$COL/empty/empty_vs_null.ru" "empty is not null" "empty equals empty"

# ---- 3. duplicates -----------------------------------------------------------
assert_program_output "col duplicates collapse" "$COL/dup_collapse.ru" "{1, 2, 3}" 3
assert_program_output "col duplicate strings" "$COL/dup_strings.ru" '{"a", "b"}'
assert_program_output "col duplicates keep equality" "$COL/dup_equality.ru" "dup equality ok"
assert_program_output "col duplicate booleans" "$COL/dup_bool.ru" "{false, true}" 2

# ---- 4. equality -------------------------------------------------------------
assert_program_output "col equality ignores order" "$COL/eq_order.ru" "order independent" "ne consistent"
assert_program_output "col inequality different sizes" "$COL/eq_different.ru" "different sizes unequal"
assert_program_output "col two empties equal" "$COL/eq_empty.ru" "both empty equal"
assert_program_output "col different kinds unequal" "$COL/eq_kind.ru" "different kinds unequal"
assert_program_output "col separate sets have own identity" "$COL/ident_separate.ru" "separate identities"
assert_program_output "col alias shares identity" "$COL/ident_alias.ru" "alias shares identity"
assert_program_output "col identity survives call" "$COL/ident_through_function.ru" "identity survives the call"

# ---- 5. contains -------------------------------------------------------------
assert_program_output "col contains numbers" "$COL/contains_basic.ru" "true" "false"
assert_program_output "col contains strings" "$COL/contains_strings.ru" "true" "false"
assert_program_output "col incompatible probe false" "$COL/contains_probe_type.ru" "incompatible probe is not a member"
assert_program_output "col contains variable element" "$COL/contains_variable.ru" "true" "false"

# ---- 6. aggregation ----------------------------------------------------------
assert_program_output "col sum" "$COL/agg_sum.ru" "100"
assert_program_output "col product" "$COL/agg_product.ru" "24"
assert_program_output "col average exact" "$COL/agg_average_exact.ru" "25"
assert_program_output "col average fractional" "$COL/agg_average_fractional.ru" "1.5"
assert_program_output "col min max" "$COL/agg_min_max.ru" "10" "30"
# Runa 1.0 (DECISIONS #116): empty aggregation is a clean RuntimeError.
col_runtime_fail "col sum of empty is an error" "$COL/agg_sum_empty.ru" \
    "sum of an empty collection is not defined"
col_runtime_fail "col product of empty is an error" "$COL/agg_product_empty.ru" \
    "product of an empty collection is not defined"
assert_program_output "col count distinct" "$COL/agg_count.ru" "3"
assert_program_output "col sum in condition" "$COL/agg_sum_negative.ru" "sum above 55"

# ---- 7. set operations -------------------------------------------------------
assert_program_output "col union" "$COL/seto_union.ru" "{1, 2, 3, 4, 5}"
assert_program_output "col intersection" "$COL/seto_intersection.ru" "{3}"
assert_program_output "col difference" "$COL/seto_difference.ru" "{1, 2}"
assert_program_output "col union deduplicates" "$COL/seto_union_dedup.ru" "{1, 2, 3}"
assert_program_output "col disjoint sets" "$COL/seto_disjoint.ru" "{}" "{1, 2}"
assert_program_output "col symmetric difference composition" "$COL/seto_symmetric_difference.ru" "{1, 2, 4, 5}"
assert_program_output "col set ops leave originals untouched" "$COL/seto_originals_unchanged.ru" "{1, 2, 3}" "{3, 4, 5}" "{1, 2, 3, 4, 5}"

# ---- 8. ordering views -------------------------------------------------------
assert_program_output "col sort ascending view" "$COL/ord_sort.ru" "10, 20, 30"
assert_program_output "col reverse descending view" "$COL/ord_reverse.ru" "40, 30, 20, 10"
assert_program_output "col string ordering views" "$COL/ord_sort_strings.ru" '"Lumo", "Runa", "Tluxe"' '"Tluxe", "Runa", "Lumo"'
assert_program_output "col first last" "$COL/ord_first_last.ru" "10" "30"
assert_program_output "col ordering views of empty" "$COL/ord_sort_empty.ru" "" ""
assert_program_output "col sort view does not mutate" "$COL/ord_view_not_mutation.ru" "{10, 20, 30}" "10, 20, 30" 3
assert_program_output "col first is min last is max" "$COL/ord_first_equals_min.ru" "first is the smallest" "last is the largest"

# ---- 9. functions ------------------------------------------------------------
assert_program_output "col passed to function" "$COL/fun_param.ru" "60"
assert_program_output "col returned from function" "$COL/fun_return.ru" "{10, 20, 30}"
assert_program_output "col nested built-in calls" "$COL/fun_nested_calls.ru" "15" 1
assert_program_output "col recursion builds set" "$COL/fun_recursion.ru" "{1, 2, 3, 4, 5}" "15" 3
assert_program_output "col two collection params" "$COL/fun_two_params.ru" "{1, 3}"
assert_program_output "col collection in class field" "$COL/fun_collection_field.ru" "30" "true"

# ---- 10. assignment / reassignment -------------------------------------------
assert_program_output "col reassignment" "$COL/reassign_basic.ru" "{4, 5, 6}"
assert_program_output "col rebind leaves alias on old value" "$COL/reassign_alias_independent.ru" "{1, 2, 3}" "{9}"
assert_program_output "col result independent of later rebind" "$COL/reassign_old_untouched_by_ops.ru" "{1, 2, 3, 4, 5}" "{100}"
assert_program_output "col dynamic retyping still works" "$COL/reassign_dynamic_types.ru" "now a string" "{3, 4}"

# ---- 11. compile-time errors -------------------------------------------------
col_compile_fail "col mixed number string literal" "$COL/errors/err_mixed_literal.ru" \
    "collection elements must share one type (found number and string)"
col_compile_fail "col mixed boolean number literal" "$COL/errors/err_mixed_bool_number.ru" \
    "collection elements must share one type (found boolean and number)"
col_compile_fail "col mixed string boolean literal" "$COL/errors/err_mixed_string_bool.ru" \
    "collection elements must share one type (found string and boolean)"
col_compile_fail "col nested collection literal" "$COL/errors/err_nested.ru" \
    "nested collections are not supported"
col_compile_fail "col null element" "$COL/errors/err_null_element.ru" \
    "null cannot be a collection element"
col_compile_fail "col sum arity zero" "$COL/errors/err_arity_sum0.ru" \
    "'sum' expects at least 1 argument(s)"
col_compile_fail "col contains arity one" "$COL/errors/err_arity_contains1.ru" \
    "'contains' expects at least 2 argument(s)"

# ---- 12. run-time errors (clean failures, no crash) ---------------------------
col_runtime_fail "col sum on strings" "$COL/errors/err_sum_non_numeric.ru" \
    "sum expects a numeric collection, got a collection of string values"
col_runtime_fail "col product on strings" "$COL/errors/err_product_non_numeric.ru" \
    "product expects a numeric collection"
col_runtime_fail "col average on booleans" "$COL/errors/err_average_non_numeric.ru" \
    "average expects a numeric collection"
col_runtime_fail "col min on strings" "$COL/errors/err_min_non_numeric.ru" \
    "min expects a numeric collection"
col_runtime_fail "col max on strings" "$COL/errors/err_max_non_numeric.ru" \
    "max expects a numeric collection"
col_runtime_fail "col min of empty" "$COL/errors/err_min_empty.ru" "min of an empty collection"
col_runtime_fail "col max of empty" "$COL/errors/err_max_empty.ru" "max of an empty collection"
col_runtime_fail "col average of empty" "$COL/errors/err_average_empty.ru" \
    "average of an empty collection is not defined"
col_runtime_fail "col first of empty" "$COL/errors/err_first_empty.ru" "first of an empty collection"
col_runtime_fail "col last of empty" "$COL/errors/err_last_empty.ru" "last of an empty collection"
col_runtime_fail "col union type mismatch" "$COL/errors/err_union_type_mismatch.ru" \
    "union expects two collections with the same element type"
col_runtime_fail "col intersection type mismatch" "$COL/errors/err_intersection_type_mismatch.ru" \
    "intersection expects two collections with the same element type"
col_runtime_fail "col difference type mismatch" "$COL/errors/err_difference_type_mismatch.ru" \
    "difference expects two collections with the same element type"
col_runtime_fail "col sum without collection" "$COL/errors/err_sum_no_collection.ru" \
    "sum expects a collection as its first argument, got number"
col_runtime_fail "col contains without collection" "$COL/errors/err_contains_no_collection.ru" \
    "contains expects a collection as its first argument, got number"
col_runtime_fail "col sort without collection" "$COL/errors/err_sort_no_collection.ru" \
    "sort expects a collection as its first argument, got number"
col_runtime_fail "col object element rejected" "$COL/errors/err_object_element.ru" \
    "collection elements must be number, float, string, or boolean values, got object"
col_runtime_fail "col arithmetic rejected" "$COL/errors/err_collection_arithmetic.ru" \
    "unsupported operand types for '+': collection and number"

# ---- 13. malformed bytecode (VM must fail cleanly, never crash) ---------------
"$RUNAC" "$COL/literals/lit_basic.ru" -o "$TMP/colmal.rbc" >/dev/null 2>&1
python3 - "$TMP/colmal.rbc" "$TMP/colpatched.rbc" <<'PYEOF'
import sys
data = open(sys.argv[1], "rb").read()
# BuildCollection = opcode 36; the fixture builds a 4-element literal.
needle = bytes([36, 4, 0, 0, 0])
idx = data.find(needle)
if idx < 0:
    sys.exit(1)
open(sys.argv[2], "wb").write(
    data[:idx] + bytes([36, 0xFF, 0xFF, 0xFF, 0xFF]) + data[idx + 5:])
PYEOF
"$RUNAVM" "$TMP/colpatched.rbc" >/dev/null 2>"$TMP/colmal.err"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "col malformed BuildCollection count rejected"; else failed "col malformed BuildCollection count rejected"; fi
contains_file "col malformed BuildCollection message" "$TMP/colmal.err" \
    "malformed bytecode: stack underflow in collection literal"

# ---- 14. versioning: v4 files are rejected by the v5 toolchain ----------------
python3 - "$TMP/colv4.rbc" <<'PYEOF'
import struct, sys
blob = b"RUNA" + struct.pack("<I", 4)
blob += struct.pack("<I", 0)   # constants
blob += struct.pack("<I", 0)   # names
blob += struct.pack("<I", 0)   # functions
blob += struct.pack("<I", 0)   # classes
blob += struct.pack("<I", 0)   # inits
blob += struct.pack("<I", 0)   # natives
blob += struct.pack("<I", 0)   # code
open(sys.argv[1], "wb").write(blob)
PYEOF
"$RUNAVM" "$TMP/colv4.rbc" >/dev/null 2>"$TMP/colv4.err"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "col v4 file rejected by v5 VM"; else failed "col v4 file rejected by v5 VM"; fi
contains_file "col v4 rejection message" "$TMP/colv4.err" "unsupported bytecode format version 4"
