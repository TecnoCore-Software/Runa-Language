
# ################################################################################
# # v0.9 — PACKAGE SYSTEM: RPR / import / cimport / native bridge                #
# ################################################################################
# All package tests live in their own sandbox so the repository tree stays
# clean; the real tools (runac/runavm/runa/rpr/nruna) drive every layer.

PKG="$TMP/pkgs"
mkdir -p "$PKG"

# ---- helper: build the math test library (pure Runa) -------------------------
make_math_lib() { # <dest-dir>
    mkdir -p "$1"
    cat >"$1/manifest.json" <<'MEOF'
{
  "name": "math",
  "version": "1.0.0",
  "type": "runa",
  "entry": "init.ru"
}
MEOF
    cat >"$1/init.ru" <<'IEOF'
base = 10
uses = 0

fun add(a, b) [
    uses = uses + 1
    return a + b
]

fun twice(n) [
    return add(n, n)
]

fun use_count() [
    return uses
]

fun base_val() [
    return base
]

class (Shape) [
    sides = 4
    static made = 0
    constructor() [
        Shape.made = Shape.made + 1
    ]
    fun area(w, h) [
        return w * h
    ]
    static fun describe() [
        return "shape"
    ]
]
IEOF
}

# ---- helper: build the native demo package (C sources + manifest) ------------
make_native_demo() { # <dest-dir>
    mkdir -p "$1/linux-x86_64" "$1/windows-x86_64" "$1/src"
    cat >"$1/manifest.json" <<'NEOF'
{
  "name": "demo",
  "version": "1.0.0",
  "type": "native",
  "native": {
    "targets": {
      "linux-x86_64": "linux-x86_64/libdemo.so"
    },
    "api": {
      "functions": [
        { "name": "nadd", "symbol": "demo_nadd", "return": "int64",
          "parameters": ["int64", "int64"] },
        { "name": "twice32", "symbol": "demo_twice32", "return": "int32",
          "parameters": ["int32"] },
        { "name": "concat", "symbol": "demo_concat", "return": "cstring",
          "parameters": ["cstring", "cstring"] },
        { "name": "ratio", "symbol": "demo_ratio", "return": "float64",
          "parameters": [] },
        { "name": "make_handle", "symbol": "demo_make_handle", "return": "pointer",
          "parameters": ["int64"] },
        { "name": "handle_value", "symbol": "demo_handle_value", "return": "int64",
          "parameters": ["pointer"] },
        { "name": "is_even", "symbol": "demo_is_even", "return": "bool",
          "parameters": ["int64"] },
        { "name": "fit8", "symbol": "demo_fit8", "return": "int8",
          "parameters": ["int64"] },
        { "name": "nothing", "symbol": "demo_nothing", "return": "void",
          "parameters": [] }
      ]
    }
  }
}
NEOF
    cat >"$1/src/demo.c" <<'CEOF'
#include <stdlib.h>
#include <string.h>

long long demo_nadd(long long a, long long b) { return a + b; }

int demo_twice32(int n) { return n * 2; }

const char* demo_concat(const char* a, const char* b) {
    size_t la = strlen(a), lb = strlen(b);
    char* out = (char*)malloc(la + lb + 1);
    memcpy(out, a, la);
    memcpy(out + la, b, lb + 1);
    return out;
}

double demo_ratio(void) { return 2.5; }

void* demo_make_handle(long long v) { return (void*)(long long)(v * 10 + 7); }

long long demo_handle_value(void* h) { return (long long)(long)h; }

int demo_is_even(long long n) { return n % 2 == 0; }

signed char demo_fit8(long long n) { return (signed char)(n % 128); }

void demo_nothing(void) { }
CEOF
    if command -v cc >/dev/null 2>&1; then
        cc -shared -fPIC -O2 "$1/src/demo.c" -o "$1/linux-x86_64/libdemo.so"
    fi
}

if command -v cc >/dev/null 2>&1; then
    HAVE_CC=1
else
    HAVE_CC=0
    echo "note: cc not found — native (cimport) tests are skipped"
fi

# ==== 110. RPR tool ============================================================
make_math_lib "$PKG/mathlib"
"$RPR" pack "$PKG/mathlib" "$PKG/math.rpr" >"$TMP/o_rpr_pack" 2>"$TMP/e_rpr_pack"; RC=$?
check_rc "r01 rpr pack succeeds" 0 "$RC"
if head -c 2 "$PKG/math.rpr" | grep -q "PK"; then pass "r02 rpr magic number (ZIP)"; else failed "r02 rpr magic number (ZIP)"; fi
"$RPR" list "$PKG/math.rpr" >"$TMP/o_rpr_list" 2>/dev/null
contains_file "r03 rpr list shows entry" "$TMP/o_rpr_list" "init.ru"
"$RPR" inspect "$PKG/math.rpr" >"$TMP/o_rpr_inspect" 2>/dev/null
contains_file "r04 inspect shows name" "$TMP/o_rpr_inspect" "name    : math"
contains_file "r05 inspect shows version" "$TMP/o_rpr_inspect" "version : 1.0.0"
"$RPR" extract "$PKG/math.rpr" "$PKG/extracted" >"$TMP/o_rpr_ext" 2>/dev/null
if [[ -f "$PKG/extracted/manifest.json" && -f "$PKG/extracted/init.ru" ]]; then
    pass "r06 rpr extract roundtrip"
else
    failed "r06 rpr extract roundtrip"
fi
"$RPR" pack "$PKG/mathlib" "$PKG/math2.rpr" >/dev/null 2>&1
if cmp -s "$PKG/math.rpr" "$PKG/math2.rpr"; then pass "r07 pack is deterministic"; else failed "r07 pack is deterministic"; fi

# malformed archives must be rejected cleanly
printf 'NOT_AN_ARCHIVE_AT_ALL' > "$PKG/bad.rpr"
"$RPR" list "$PKG/bad.rpr" >/dev/null 2>"$TMP/e_rpr_bad"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "r08 bad magic rejected"; else failed "r08 bad magic rejected"; fi
contains_file "r09 bad magic message" "$TMP/e_rpr_bad" "ZIP container"

# legacy RPRF containers are rejected with an explicit, honest message
# (DECISIONS #119: reject, no migration)
python3 - "$PKG/badver.rpr" <<'PYEOF'
import struct, sys
blob = b"RPRF" + struct.pack("<I", 99) + struct.pack("<I", 0) + struct.pack("<I", 0) + struct.pack("<I", 0)
open(sys.argv[1], "wb").write(blob)
PYEOF
"$RPR" list "$PKG/badver.rpr" >/dev/null 2>"$TMP/e_rpr_ver"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "r10 legacy RPRF rejected"; else failed "r10 legacy RPRF rejected"; fi
contains_file "r11 legacy RPRF message" "$TMP/e_rpr_ver" "old RPRF container format is not supported"

# path traversal entry (crafted ZIP archive with a '..' path)
python3 - "$PKG/trav.rpr" <<'PYEOF'
import json, sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_STORED) as z:
    z.writestr("manifest.json", json.dumps({"name": "m", "version": "1.0.0",
                                            "type": "runa", "entry": "init.ru"}))
    z.writestr("../evil.ru", b"evil")
PYEOF
"$RPR" list "$PKG/trav.rpr" >/dev/null 2>"$TMP/e_rpr_trav"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "r12 path traversal rejected"; else failed "r12 path traversal rejected"; fi
contains_file "r13 path traversal message" "$TMP/e_rpr_trav" "path traversal in archive path '../evil.ru'"

# truncated archive
head -c 10 "$PKG/math.rpr" > "$PKG/trunc.rpr"
"$RPR" list "$PKG/trunc.rpr" >/dev/null 2>"$TMP/e_rpr_trunc"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "r14 truncated archive rejected"; else failed "r14 truncated archive rejected"; fi

# ==== 120. normal import =======================================================
mkdir -p "$PKG/p1/runalibs"
cp "$PKG/math.rpr" "$PKG/p1/runalibs/math.rpr"
cat > "$PKG/p1/main.ru" <<'EOF'
import math

printr(math.add(2, 3))
printr(math.twice(21))
EOF
"$RUNA" "$PKG/p1/main.ru" >"$TMP/o_i1" 2>/dev/null
line_is "i1 imported function call" "$TMP/o_i1" "5"
line_is "i1b nested call inside library" "$TMP/o_i1" "42"

cat > "$PKG/p1/sel.ru" <<'EOF'
as "math" import {add, Shape}

printr(add(40, 2))
s = new Shape()
if s is Shape [
    printr(s.area(6, 7))
]
printr(Shape.describe())
printr(Shape.made)
EOF
"$RUNA" "$PKG/p1/sel.ru" >"$TMP/o_i2" 2>/dev/null
line_is "i2 selective function" "$TMP/o_i2" "42"
line_is "i2b selective class instance" "$TMP/o_i2" "42"
line_is "i2c selective static method" "$TMP/o_i2" "shape"
line_is "i2d selective static field" "$TMP/o_i2" "1"

cat > "$PKG/p1/init.ru" <<'EOF'
import math
printr(math.twice(5))
printr(Shape.made)
EOF
"$RUNA" "$PKG/p1/init.ru" >/dev/null 2>"$TMP/e_i3"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "i3 unimported symbol rejected"; else failed "i3 unimported symbol rejected"; fi
contains_file "i3b message" "$TMP/e_i3" "undefined variable 'Shape'"

# missing package
mkdir -p "$PKG/p2/runalibs"
cat > "$PKG/p2/main.ru" <<'EOF'
import nosuchpkg
EOF
"$RUNA" "$PKG/p2/main.ru" >/dev/null 2>"$TMP/e_i4"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "i4 missing package fails"; else failed "i4 missing package fails"; fi
contains_file "i4b package not found message" "$TMP/e_i4" "package not found: 'nosuchpkg'"

# missing entry file (name stays consistent so the name check passes)
mkdir -p "$PKG/noentry"
printf '{"name":"nosuchpkg","version":"1.0.0","type":"runa","entry":"start.ru"}\n' > "$PKG/noentry/manifest.json"
printf 'x = 1\n' > "$PKG/noentry/init.ru"
"$RPR" pack "$PKG/noentry" "$PKG/noentry.rpr" >/dev/null 2>&1
cp "$PKG/noentry.rpr" "$PKG/p2/runalibs/nosuchpkg.rpr"
"$RUNA" "$PKG/p2/main.ru" >/dev/null 2>"$TMP/e_i5"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "i5 missing entry fails"; else failed "i5 missing entry fails"; fi
contains_file "i5b entry message" "$TMP/e_i5" "library entry 'start.ru' not found"

# invalid manifest (rpr pack refuses to build it)
mkdir -p "$PKG/badman"
printf 'this is not json' > "$PKG/badman/manifest.json"
printf 'x = 1\n' > "$PKG/badman/init.ru"
"$RPR" pack "$PKG/badman" "$PKG/badman.rpr" >/dev/null 2>"$TMP/e_rpr_man"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "i6 pack rejects invalid manifest"; else failed "i6 pack rejects invalid manifest"; fi

# manifest name mismatch
make_math_lib "$PKG/mismatch"
python3 - "$PKG/mismatch/manifest.json" <<'PYEOF'
import json, sys
m = json.load(open(sys.argv[1]))
m["name"] = "other"
json.dump(m, open(sys.argv[1], "w"))
PYEOF
"$RPR" pack "$PKG/mismatch" "$PKG/mismatch.rpr" >/dev/null 2>&1
cp "$PKG/mismatch.rpr" "$PKG/p2/runalibs/math.rpr"
cat > "$PKG/p2/mismatch.ru" <<'EOF'
import math
EOF
"$RUNA" "$PKG/p2/mismatch.ru" >/dev/null 2>"$TMP/e_i7"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "i7 name mismatch fails"; else failed "i7 name mismatch fails"; fi
contains_file "i7b mismatch message" "$TMP/e_i7" "declares \"other\""

# missing selective symbol
make_math_lib "$PKG/mathok"
"$RPR" pack "$PKG/mathok" "$PKG/mathok.rpr" >/dev/null 2>&1
cp "$PKG/mathok.rpr" "$PKG/p2/runalibs/math.rpr"
cat > "$PKG/p2/missingsym.ru" <<'EOF'
as "math" import {nothere}
EOF
"$RUNA" "$PKG/p2/missingsym.ru" >/dev/null 2>"$TMP/e_i8"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "i8 missing symbol fails"; else failed "i8 missing symbol fails"; fi
contains_file "i8b missing symbol message" "$TMP/e_i8" "imported symbol not found: 'math.nothere'"

# unknown namespace member
cat > "$PKG/p2/unknownmember.ru" <<'EOF'
import math
printr(math.nothere())
EOF
"$RUNA" "$PKG/p2/unknownmember.ru" >/dev/null 2>"$TMP/e_i9"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "i9 unknown member fails"; else failed "i9 unknown member fails"; fi
contains_file "i9b unknown member message" "$TMP/e_i9" "package 'math' has no function 'nothere'"

# arity mismatch across the boundary
cat > "$PKG/p2/arity.ru" <<'EOF'
import math
printr(math.add(1))
EOF
"$RUNA" "$PKG/p2/arity.ru" >/dev/null 2>"$TMP/e_i10"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "i10 arity mismatch fails"; else failed "i10 arity mismatch fails"; fi
contains_file "i10b arity message" "$TMP/e_i10" "'math.add' expects 2 argument(s), got 1"

# import cycle
mkdir -p "$PKG/cyca" "$PKG/cycb"
cat > "$PKG/cyca/manifest.json" <<'EOF'
{"name":"cyca","version":"1.0.0","type":"runa","entry":"init.ru"}
EOF
cat > "$PKG/cyca/init.ru" <<'EOF'
import cycb
fun a() [
    return 1
]
EOF
cat > "$PKG/cycb/manifest.json" <<'EOF'
{"name":"cycb","version":"1.0.0","type":"runa","entry":"init.ru"}
EOF
cat > "$PKG/cycb/init.ru" <<'EOF'
import cyca
fun b() [
    return 2
]
EOF
"$RPR" pack "$PKG/cyca" "$PKG/cyca.rpr" >/dev/null 2>&1
"$RPR" pack "$PKG/cycb" "$PKG/cycb.rpr" >/dev/null 2>&1
cp "$PKG/cyca.rpr" "$PKG/cycb.rpr" "$PKG/p2/runalibs/"
cat > "$PKG/p2/cycle.ru" <<'EOF'
import cyca
EOF
"$RUNA" "$PKG/p2/cycle.ru" >/dev/null 2>"$TMP/e_i11"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "i11 import cycle fails"; else failed "i11 import cycle fails"; fi
contains_file "i11b cycle message" "$TMP/e_i11" "import cycle detected"

# package chains: main -> upper -> math
mkdir -p "$PKG/upper"
cat > "$PKG/upper/manifest.json" <<'EOF'
{"name":"upper","version":"1.0.0","type":"runa","entry":"init.ru"}
EOF
cat > "$PKG/upper/init.ru" <<'EOF'
import math
fun upper_twice(n) [
    return math.twice(n) + 1
]
EOF
"$RPR" pack "$PKG/upper" "$PKG/upper.rpr" >/dev/null 2>&1
cp "$PKG/upper.rpr" "$PKG/p2/runalibs/"
cat > "$PKG/p2/chain.ru" <<'EOF'
import upper
import math
printr(upper.upper_twice(10))
printr(math.base_val())
EOF
"$RUNA" "$PKG/p2/chain.ru" >"$TMP/o_i12" 2>/dev/null
line_is "i12 package chain" "$TMP/o_i12" "21"
line_is "i12b package global readable from its functions" "$TMP/o_i12" "10"

# duplicate import of the same symbol
cat > "$PKG/p2/dupsym.ru" <<'EOF'
as "math" import {add}
as "math" import {add}
EOF
"$RUNA" "$PKG/p2/dupsym.ru" >/dev/null 2>"$TMP/e_i13"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "i13 duplicate selective symbol fails"; else failed "i13 duplicate selective symbol fails"; fi

# native-typed package via import is rejected
if [[ "$HAVE_CC" -eq 1 ]]; then
    make_native_demo "$PKG/demolib"
    "$RPR" pack "$PKG/demolib" "$PKG/demolib.rpr" >/dev/null 2>&1
    cp "$PKG/demolib.rpr" "$PKG/p2/runalibs/demo.rpr"
    cat > "$PKG/p2/natimport.ru" <<'EOF'
import demo
EOF
    "$RUNA" "$PKG/p2/natimport.ru" >/dev/null 2>"$TMP/e_i14"; RC=$?
    if [[ "$RC" -ne 0 ]]; then pass "i14 native package via import rejected"; else failed "i14 native package via import rejected"; fi
    contains_file "i14b message" "$TMP/e_i14" "cannot be used with 'import'"
fi

# import inside a function is a CompileError
cat > "$PKG/p2/nested.ru" <<'EOF'
fun f() [
    import math
]
EOF
"$RUNA" "$PKG/p2/nested.ru" >/dev/null 2>"$TMP/e_i15"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "i15 nested import rejected"; else failed "i15 nested import rejected"; fi
contains_file "i15b message" "$TMP/e_i15" "only allowed at the top level"

# package names are reserved
cat > "$PKG/p2/reserved.ru" <<'EOF'
import math
math = 5
EOF
"$RUNA" "$PKG/p2/reserved.ru" >/dev/null 2>"$TMP/e_i16"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "i16 package name reserved"; else failed "i16 package name reserved"; fi
contains_file "i16b message" "$TMP/e_i16" "imported package or symbol name"

# imported function is not a value
cat > "$PKG/p2/notvalue.ru" <<'EOF'
import math
f = math.add
EOF
"$RUNA" "$PKG/p2/notvalue.ru" >/dev/null 2>"$TMP/e_i17"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "i17 imported function not a value"; else failed "i17 imported function not a value"; fi
contains_file "i17b message" "$TMP/e_i17" "cannot be used as a value"

# library globals are invisible to the importer (isolation)
cat > "$PKG/p2/isolation.ru" <<'EOF'
import math
printr(base)
EOF
"$RUNA" "$PKG/p2/isolation.ru" >/dev/null 2>"$TMP/e_i18"; RC=$?
if [[ "$RC" -ne 0 ]]; then pass "i18 library globals isolated"; else failed "i18 library globals isolated"; fi
contains_file "i18b message" "$TMP/e_i18" "undefined variable 'base'"

# precedence: the project runalibs wins over the toolchain's external libs.
# A distinct 'upper' is planted next to the real executable (removed after).
mkdir -p "$PKG/exeupper" "$PKG/p3/runalibs" "$ROOT/build/runalibs/external"
cat > "$PKG/exeupper/manifest.json" <<'EOF'
{"name":"upper","version":"1.0.0","type":"runa","entry":"init.ru"}
EOF
cat > "$PKG/exeupper/init.ru" <<'EOF'
fun upper_twice(n) [
    return 999
]
EOF
"$RPR" pack "$PKG/exeupper" "$ROOT/build/runalibs/external/upper.rpr" >/dev/null 2>&1
cp "$PKG/upper.rpr" "$PKG/p3/runalibs/upper.rpr"
cp "$PKG/mathok.rpr" "$PKG/p3/runalibs/math.rpr"
cat > "$PKG/p3/prec.ru" <<'EOF'
import upper
printr(upper.upper_twice(1))
EOF
"$RUNA" "$PKG/p3/prec.ru" >"$TMP/o_i19" 2>/dev/null
RC_I19=$?
rm -f "$ROOT/build/runalibs/external/upper.rpr"
line_is "i19 project runalibs precedence" "$TMP/o_i19" "3"

# two packages may both declare the same function name (hidden isolation)
mkdir -p "$PKG/twin1" "$PKG/twin2"
cat > "$PKG/twin1/manifest.json" <<'EOF'
{"name":"twin1","version":"1.0.0","type":"runa","entry":"init.ru"}
EOF
cat > "$PKG/twin1/init.ru" <<'EOF'
fun who() [
    return "one"
]
EOF
cat > "$PKG/twin2/manifest.json" <<'EOF'
{"name":"twin2","version":"1.0.0","type":"runa","entry":"init.ru"}
EOF
cat > "$PKG/twin2/init.ru" <<'EOF'
fun who() [
    return "two"
]
EOF
"$RPR" pack "$PKG/twin1" "$PKG/twin1.rpr" >/dev/null 2>&1
"$RPR" pack "$PKG/twin2" "$PKG/twin2.rpr" >/dev/null 2>&1
cp "$PKG/twin1.rpr" "$PKG/twin2.rpr" "$PKG/p3/runalibs/"
cat > "$PKG/p3/twins.ru" <<'EOF'
import twin1
import twin2
printr(twin1.who())
printr(twin2.who())
EOF
"$RUNA" "$PKG/p3/twins.ru" >"$TMP/o_i20" 2>/dev/null
line_is "i20 twin package isolation a" "$TMP/o_i20" "one"
line_is "i20b twin package isolation b" "$TMP/o_i20" "two"

# runac + runavm split flow with imports
"$RUNAC" "$PKG/p1/main.ru" -o "$TMP/p1main.rbc" >"$TMP/o_i21" 2>/dev/null; RC=$?
check_rc "i21 runac compiles with imports" 0 "$RC"
"$RUNAVM" "$TMP/p1main.rbc" >"$TMP/o_i22" 2>/dev/null
line_is "i21b split run matches" "$TMP/o_i22" "5"

# ==== 130. cimport + native bridge =============================================
if [[ "$HAVE_CC" -eq 1 ]]; then
    make_native_demo "$PKG/p4/native/demo"
    cat > "$PKG/p4/nat.ru" <<'EOF'
cimport native/demo

printr(demo.nadd(20, 22))
printr(demo.twice32(21))
printr(demo.concat("سلام ", "رونا"))
printr(demo.ratio())
h = demo.make_handle(5)
if h === null [
    printr("null handle")
] else [
    printr(demo.handle_value(h))
]
printr(demo.is_even(10))
printr(demo.is_even(7))
printr(demo.fit8(130))
demo.nothing()
printr("done")
nullh = demo.handle_value(null)
printr(nullh)
EOF
    "$RUNA" "$PKG/p4/nat.ru" >"$TMP/o_n1" 2>/dev/null
    line_is "n1 native int64 call" "$TMP/o_n1" "42"
    line_is "n1b native int32 call" "$TMP/o_n1" "42"
    line_is "n1c native cstring (UTF-8)" "$TMP/o_n1" "سلام رونا"
    line_is "n1d native float64 return" "$TMP/o_n1" "2.5"
    line_is "n1e pointer roundtrip" "$TMP/o_n1" "57"
    line_is "n1f bool true" "$TMP/o_n1" "true"
    line_is "n1g bool false" "$TMP/o_n1" "false"
    line_is "n1h int8 wraps into range" "$TMP/o_n1" "2"
    line_is "n1i void call runs" "$TMP/o_n1" "done"
    line_is "n1j null pointer maps to null" "$TMP/o_n1" "0"

    # split flow: runac + runavm with cimport
    "$RUNAC" "$PKG/p4/nat.ru" -o "$PKG/p4/nat.rbc" >/dev/null 2>"$TMP/e_n2"; RC=$?
    check_rc "n2 runac compiles cimport" 0 "$RC"
    "$RUNAVM" "$PKG/p4/nat.rbc" >"$TMP/o_n2b" 2>/dev/null
    line_is "n2b split native run" "$TMP/o_n2b" "42"

    # garbage binary → clean runtime error
    mkdir -p "$PKG/p5/native/broken/linux-x86_64"
    cat > "$PKG/p5/native/broken/manifest.json" <<'EOF'
{ "name": "broken", "version": "1.0.0", "type": "native",
  "native": { "targets": { "linux-x86_64": "linux-x86_64/libbroken.so" },
    "api": { "functions": [ { "name": "go", "symbol": "b_go", "return": "int64",
      "parameters": [] } ] } } }
EOF
    echo "NOT_A_REAL_SHARED_OBJECT" > "$PKG/p5/native/broken/linux-x86_64/libbroken.so"
    cat > "$PKG/p5/bad.ru" <<'EOF'
cimport native/broken
printr(broken.go())
EOF
    "$RUNA" "$PKG/p5/bad.ru" >/dev/null 2>"$TMP/e_n3"; RC=$?
    if [[ "$RC" -ne 0 ]]; then pass "n3 garbage binary fails"; else failed "n3 garbage binary fails"; fi
    contains_file "n3b message" "$TMP/e_n3" "native library load failed"

    # missing manifest
    mkdir -p "$PKG/p5/native/ghost"
    cat > "$PKG/p5/ghost.ru" <<'EOF'
cimport native/ghost
EOF
    "$RUNA" "$PKG/p5/ghost.ru" >/dev/null 2>"$TMP/e_n4"; RC=$?
    if [[ "$RC" -ne 0 ]]; then pass "n4 missing manifest fails"; else failed "n4 missing manifest fails"; fi
    contains_file "n4b message" "$TMP/e_n4" "native manifest not found"

    # missing current-platform target
    mkdir -p "$PKG/p5/native/onlywin/windows-x86_64"
    cat > "$PKG/p5/native/onlywin/manifest.json" <<'EOF'
{ "name": "onlywin", "version": "1.0.0", "type": "native",
  "native": { "targets": { "windows-x86_64": "windows-x86_64/onlywin.dll" },
    "api": { "functions": [ { "name": "go", "symbol": "o_go", "return": "int64",
      "parameters": [] } ] } } }
EOF
    cat > "$PKG/p5/onlywin.ru" <<'EOF'
cimport native/onlywin
EOF
    "$RUNA" "$PKG/p5/onlywin.ru" >/dev/null 2>"$TMP/e_n5"; RC=$?
    if [[ "$RC" -ne 0 ]]; then pass "n5 missing target fails"; else failed "n5 missing target fails"; fi
    contains_file "n5b message" "$TMP/e_n5" "native target not found for platform"
    contains_file "n5c lists available" "$TMP/e_n5" "available: windows-x86_64"

    # missing binary for an existing target
    mkdir -p "$PKG/p5/native/nobin/linux-x86_64"
    cat > "$PKG/p5/native/nobin/manifest.json" <<'EOF'
{ "name": "nobin", "version": "1.0.0", "type": "native",
  "native": { "targets": { "linux-x86_64": "linux-x86_64/libnobin.so" },
    "api": { "functions": [ { "name": "go", "symbol": "b_go", "return": "int64",
      "parameters": [] } ] } } }
EOF
    cat > "$PKG/p5/nobin.ru" <<'EOF'
cimport native/nobin
EOF
    "$RUNA" "$PKG/p5/nobin.ru" >/dev/null 2>"$TMP/e_n6"; RC=$?
    if [[ "$RC" -ne 0 ]]; then pass "n6 missing binary fails"; else failed "n6 missing binary fails"; fi
    contains_file "n6b message" "$TMP/e_n6" "native binary missing"

    # float parameter → compile-time "not supported"
    mkdir -p "$PKG/p5/native/flt/linux-x86_64"
    cat > "$PKG/p5/native/flt/manifest.json" <<'EOF'
{ "name": "flt", "version": "1.0.0", "type": "native",
  "native": { "targets": { "linux-x86_64": "linux-x86_64/libflt.so" },
    "api": { "functions": [ { "name": "half", "symbol": "f_half", "return": "float64",
      "parameters": ["float64"] } ] } } }
EOF
    printf 'V=1' > "$PKG/p5/native/flt/linux-x86_64/libflt.so"
    cat > "$PKG/p5/flt.ru" <<'EOF'
cimport native/flt
EOF
    "$RUNA" "$PKG/p5/flt.ru" >/dev/null 2>"$TMP/e_n7"; RC=$?
    if [[ "$RC" -ne 0 ]]; then pass "n7 float parameter rejected"; else failed "n7 float parameter rejected"; fi
    contains_file "n7b message" "$TMP/e_n7" "parameter type 'float64' is not supported"

    # unknown function in a cimport namespace
    mkdir -p "$PKG/p5/native"
    cp -r "$PKG/p4/native/demo" "$PKG/p5/native/demo/"
    cat > "$PKG/p5/unkfn.ru" <<'EOF'
cimport native/demo
printr(demo.nothere())
EOF
    "$RUNA" "$PKG/p5/unkfn.ru" >/dev/null 2>"$TMP/e_n8"; RC=$?
    if [[ "$RC" -ne 0 ]]; then pass "n8 unknown native function fails"; else failed "n8 unknown native function fails"; fi
    contains_file "n8b message" "$TMP/e_n8" "has no function 'nothere'"

    # arity mismatch on a native call (compile time)
    cat > "$PKG/p5/aritynat.ru" <<'EOF'
cimport native/demo
printr(demo.nadd(1))
EOF
    "$RUNA" "$PKG/p5/aritynat.ru" >/dev/null 2>"$TMP/e_n9"; RC=$?
    if [[ "$RC" -ne 0 ]]; then pass "n9 native arity mismatch fails"; else failed "n9 native arity mismatch fails"; fi

    # runtime type mismatch: string where int64 is declared
    cat > "$PKG/p5/typemix.ru" <<'EOF'
cimport native/demo
printr(demo.nadd("a", "b"))
EOF
    "$RUNA" "$PKG/p5/typemix.ru" >/dev/null 2>"$TMP/e_n10"; RC=$?
    if [[ "$RC" -ne 0 ]]; then pass "n10 native type mismatch fails"; else failed "n10 native type mismatch fails"; fi
    contains_file "n10b message" "$TMP/e_n10" "must be a number"

    # out-of-range argument
    cat > "$PKG/p5/range.ru" <<'EOF'
cimport native/demo
printr(demo.twice32(9999999999))
EOF
    "$RUNA" "$PKG/p5/range.ru" >/dev/null 2>"$TMP/e_n11"; RC=$?
    if [[ "$RC" -ne 0 ]]; then pass "n11 int32 range check"; else failed "n11 int32 range check"; fi
    contains_file "n11b message" "$TMP/e_n11" "int32 range"

    # import must NOT load a native binary (spec section 35)
    mkdir -p "$PKG/p6/runalibs"
    python3 - "$PKG/p6/runalibs/natpkg.rpr" <<'PYEOF'
import json, sys, zipfile
# A hybrid package whose init.ru does NOT cimport: its manifest claims a
# native capability, but plain `import` must leave it dormant.
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_STORED) as z:
    z.writestr("manifest.json", json.dumps({"name": "natpkg", "version": "1.0.0",
                                            "type": "hybrid", "entry": "init.ru"}))
    z.writestr("init.ru", b"fun label() [\n    return \"runa-only\"\n]\n")
PYEOF
    cat > "$PKG/p6/dormant.ru" <<'EOF'
import natpkg
printr(natpkg.label())
EOF
    "$RUNA" "$PKG/p6/dormant.ru" >"$TMP/o_n12" 2>/dev/null
    line_is "n12 import does not load natives" "$TMP/o_n12" "runa-only"

    # hybrid: a library cimports ITS OWN embedded backend (package context)
    mkdir -p "$PKG/hybrid/native/backend/linux-x86_64"
    cp "$PKG/p4/native/demo/linux-x86_64/libdemo.so" "$PKG/hybrid/native/backend/linux-x86_64/libbackend.so"
    cat > "$PKG/hybrid/manifest.json" <<'EOF'
{"name":"hybrid","version":"1.0.0","type":"hybrid","entry":"init.ru"}
EOF
    cat > "$PKG/hybrid/init.ru" <<'EOF'
cimport native/backend

fun combine(a, b) [
    return backend.nadd(a, b)
]
EOF
    cat > "$PKG/hybrid/native/backend/manifest.json" <<'EOF'
{ "name": "backend", "version": "1.0.0", "type": "native",
  "native": { "targets": { "linux-x86_64": "linux-x86_64/libbackend.so" },
    "api": { "functions": [ { "name": "nadd", "symbol": "demo_nadd",
      "return": "int64", "parameters": ["int64", "int64"] } ] } } }
EOF
    "$RPR" pack "$PKG/hybrid" "$PKG/hybrid.rpr" >/dev/null 2>&1
    mkdir -p "$PKG/p7/runalibs"
    cp "$PKG/hybrid.rpr" "$PKG/p7/runalibs/"
    cat > "$PKG/p7/hybridmain.ru" <<'EOF'
import hybrid
printr(hybrid.combine(40, 2))
EOF
    (cd "$PKG/p7" && "$RUNA" hybridmain.ru) >"$TMP/o_n13" 2>"$TMP/e_n13"
    line_is "n13 package-relative cimport" "$TMP/o_n13" "42"

    # resolution is independent of the working directory
    (cd / && "$RUNA" "$PKG/p7/hybridmain.ru") >"$TMP/o_n14" 2>"$TMP/e_n14"
    line_is "n14 cwd-independent resolution" "$TMP/o_n14" "42"

    # duplicate cimport is a CompileError
    cat > "$PKG/p4/dupcimp.ru" <<'EOF'
cimport native/demo
cimport native/demo
EOF
    "$RUNA" "$PKG/p4/dupcimp.ru" >/dev/null 2>"$TMP/e_n15"; RC=$?
    if [[ "$RC" -ne 0 ]]; then pass "n15 duplicate cimport fails"; else failed "n15 duplicate cimport fails"; fi
    contains_file "n15b message" "$TMP/e_n15" "duplicate cimport"

    # cimport inside a function is a CompileError
    cat > "$PKG/p4/funcimp.ru" <<'EOF'
fun f() [
    cimport native/demo
]
EOF
    "$RUNA" "$PKG/p4/funcimp.ru" >/dev/null 2>"$TMP/e_n16"; RC=$?
    if [[ "$RC" -ne 0 ]]; then pass "n16 nested cimport rejected"; else failed "n16 nested cimport rejected"; fi
    contains_file "n16b message" "$TMP/e_n16" "only allowed at the top level"

    # selective { } syntax must NOT extend to cimport (spec section 18)
    cat > "$PKG/p4/selcimp.ru" <<'EOF'
as "native/demo" cimport {nadd}
EOF
    "$RUNA" "$PKG/p4/selcimp.ru" >/dev/null 2>"$TMP/e_n17"; RC=$?
    if [[ "$RC" -ne 0 ]]; then pass "n17 selective cimport rejected"; else failed "n17 selective cimport rejected"; fi

    # nruna roundtrip: init → build → check → use
    (cd "$TMP" && "$NRUNA" init nrtool) >"$TMP/o_n18" 2>/dev/null; RC=$?
    check_rc "n18 nruna init" 0 "$RC"
    if [[ -f "$TMP/native/nrtool/manifest.json" ]]; then pass "n19 scaffold manifest exists"; else failed "n19 scaffold manifest exists"; fi
    (cd "$TMP" && "$NRUNA" build native/nrtool) >"$TMP/o_n20" 2>/dev/null; RC=$?
    check_rc "n20 nruna build" 0 "$RC"
    (cd "$TMP" && "$NRUNA" check native/nrtool) >"$TMP/o_n21" 2>/dev/null; RC=$?
    check_rc "n21 nruna check resolves symbols" 0 "$RC"
    contains_file "n21b check output" "$TMP/o_n21" "symbols resolved"

    mkdir -p "$PKG/p8/native"
    cp -r "$TMP/native/nrtool" "$PKG/p8/native/"
    cat > "$PKG/p8/usetool.ru" <<'EOF'
cimport native/nrtool
printr(nrtool.ping())
EOF
    "$RUNA" "$PKG/p8/usetool.ru" >"$TMP/o_n22" 2>/dev/null
    line_is "n22 nruna-built package usable" "$TMP/o_n22" "1"
fi
