# Spec-tree runner (Runa 1.0, master spec section 102 + DECISIONS #121).
# Executes every fixture in tests/spec/<area>/: positive programs compare
# stdout against X.expected, negative programs (err_X) must fail with the
# needle from err_X.needle — at compile time when the compiler can prove it,
# at run time otherwise.
SPEC_DIR="$ROOT/tests/spec"

spec_run_all() {
    local pass_count=0 fail_count=0
    for ru in "$SPEC_DIR"/*/*.ru; do
        local base="${ru%.ru}"
        local label="spec ${ru#$SPEC_DIR/}"
        if [[ -f "$base.expected" ]]; then
            # ---- positive fixture: run and compare stdout -------------------
            if "$RUNAC" "$ru" -o "$TMP/spec.rbc" >/dev/null 2>"$TMP/spec.ce" &&
               "$RUNAVM" "$TMP/spec.rbc" >"$TMP/spec.out" 2>/dev/null; then
                if diff -q "$TMP/spec.out" "$base.expected" >/dev/null 2>&1; then
                    pass "spec $label"
                    pass_count=$((pass_count + 1))
                else
                    failed "spec $label (output mismatch)"
                    diff "$base.expected" "$TMP/spec.out" | head -4
                    fail_count=$((fail_count + 1))
                fi
            else
                failed "spec $label (compile/run failed: $(head -1 "$TMP/spec.ce" 2>/dev/null))"
                fail_count=$((fail_count + 1))
            fi
        else
            # ---- negative fixture: must fail with the recorded needle --------
            local needle
            needle=$(cat "$base.needle")
            local ok=0
            if "$RUNAC" "$ru" -o "$TMP/spec.rbc" >/dev/null 2>"$TMP/spec.ce"; then
                if "$RUNAVM" "$TMP/spec.rbc" >/dev/null 2>"$TMP/spec.ee"; then
                    ok=1  # ran successfully — a negative fixture must not
                    failed "spec $label (ran successfully, expected: $needle)"
                else
                    if grep -qF -- "$needle" "$TMP/spec.ee"; then
                        pass "spec $label"
                        pass_count=$((pass_count + 1))
                    else
                        failed "spec $label (missing: $needle)"
                        head -2 "$TMP/spec.ee"
                        fail_count=$((fail_count + 1))
                    fi
                fi
            else
                if grep -qF -- "$needle" "$TMP/spec.ce"; then
                    pass "spec $label"
                    pass_count=$((pass_count + 1))
                else
                    failed "spec $label (missing: $needle)"
                    head -2 "$TMP/spec.ce"
                    fail_count=$((fail_count + 1))
                fi
            fi
            [[ "$ok" -eq 1 ]] && fail_count=$((fail_count + 1))
        fi
    done
    echo "        spec tree: $pass_count passed, $fail_count failed"
}

# Cyclic object-graph lifetime (spec section 102: mandatory lifetime test).
# With pure reference counting these cycles would never be reclaimed and the
# runtime would report collections=0. RUNA_GC_STATS prints one deterministic
# stderr line: "[runa gc] collections=<N> live=<M>".
spec_gc_test() {
    local src="$SPEC_DIR/lifetime/cycles_reclaimed.ru"
    if RUNA_GC_STATS=1 "$RUNA" "$src" >"$TMP/gc.out" 2>"$TMP/gc.err" &&
       grep -q "collections=1" "$TMP/gc.err" &&
       grep -q "survived" "$TMP/gc.out"; then
        pass "spec lifetime/cyclic graphs reclaimed by reachability"
    else
        failed "spec lifetime/cyclic graphs reclaimed by reachability"
        head -2 "$TMP/gc.err"
    fi
}
spec_gc_test
