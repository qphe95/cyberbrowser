#!/usr/bin/env python3
"""
gc_store_check.py - find stores of GC references that skip the dirty flag.

On Linux and macOS the concurrent collector learns about heap writes from
software dirty flags (one byte per 4 KB card, set by gc_dirty_slot /
gc_dirty_range and the write-helper macros).  A store of a handle or a
GCValue into memory that may be a GC object, with no dirty mark after it,
can hide a reference from concurrent marking or from the nursery's
remembered set.  This tool lists such stores.

It parses each translation unit with libclang (through ctypes, so no Python
package is needed) and reports every assignment, memcpy/memmove and atomic
store whose destination
  * has a reference type: GCValue, GCHandle or a typedef of either, or a
    struct/union/array containing one, and
  * is not a local variable or parameter (it is reached through a pointer),
unless the store is covered: followed in the same block by a dirty-marking
call on the same object, or made inside a write-helper macro.

usage: gc_store_check.py [--lib libclang.so] [--list] compile_commands.json [files...]
Exit status 1 if uncovered stores were found.
"""
import ctypes as C, json, os, sys, re

LIB_CANDIDATES = [
    os.environ.get("LIBCLANG", ""),
    "/usr/lib/llvm-18/lib/libclang-18.so.1", "/usr/lib/llvm-18/lib/libclang.so.1",
    "/usr/lib/llvm-17/lib/libclang.so.1", "/usr/lib/llvm-19/lib/libclang.so.1",
    "/opt/homebrew/opt/llvm/lib/libclang.dylib", "/usr/local/opt/llvm/lib/libclang.dylib",
    "C:/Program Files/LLVM/bin/libclang.dll",
]

class CXString(C.Structure):
    _fields_ = [("data", C.c_void_p), ("flags", C.c_uint)]
class CXCursor(C.Structure):
    _fields_ = [("kind", C.c_int), ("xdata", C.c_int), ("data", C.c_void_p * 3)]
class CXType(C.Structure):
    _fields_ = [("kind", C.c_int), ("data", C.c_void_p * 2)]
class CXSourceLocation(C.Structure):
    _fields_ = [("ptr_data", C.c_void_p * 2), ("int_data", C.c_uint)]
class CXSourceRange(C.Structure):
    _fields_ = [("ptr_data", C.c_void_p * 2), ("begin_int_data", C.c_uint), ("end_int_data", C.c_uint)]
class CXToken(C.Structure):
    _fields_ = [("int_data", C.c_uint * 4), ("ptr_data", C.c_void_p)]

VISITOR = C.CFUNCTYPE(C.c_int, CXCursor, CXCursor, C.c_void_p)
FIELD_VISITOR = C.CFUNCTYPE(C.c_int, CXCursor, C.c_void_p)

def load(path):
    lib = C.CDLL(path)
    sig = {
        "clang_createIndex": (C.c_void_p, [C.c_int, C.c_int]),
        "clang_parseTranslationUnit": (C.c_void_p, [C.c_void_p, C.c_char_p, C.POINTER(C.c_char_p), C.c_int, C.c_void_p, C.c_uint, C.c_uint]),
        "clang_getTranslationUnitCursor": (CXCursor, [C.c_void_p]),
        "clang_visitChildren": (C.c_uint, [CXCursor, VISITOR, C.c_void_p]),
        "clang_getCursorKind": (C.c_int, [CXCursor]),
        "clang_getCursorType": (CXType, [CXCursor]),
        "clang_getTypeSpelling": (CXString, [CXType]),
        "clang_getCString": (C.c_char_p, [CXString]),
        "clang_disposeString": (None, [CXString]),
        "clang_getCanonicalType": (CXType, [CXType]),
        "clang_getTypeDeclaration": (CXCursor, [CXType]),
        "clang_getTypedefDeclUnderlyingType": (CXType, [CXCursor]),
        "clang_Type_getNamedType": (CXType, [CXType]),
        "clang_getPointeeType": (CXType, [CXType]),
        "clang_getArrayElementType": (CXType, [CXType]),
        "clang_getElementType": (CXType, [CXType]),
        "clang_Type_visitFields": (C.c_uint, [CXType, FIELD_VISITOR, C.c_void_p]),
        "clang_getCursorLocation": (CXSourceLocation, [CXCursor]),
        "clang_getSpellingLocation": (None, [CXSourceLocation, C.POINTER(C.c_void_p), C.POINTER(C.c_uint), C.POINTER(C.c_uint), C.POINTER(C.c_uint)]),
        "clang_getExpansionLocation": (None, [CXSourceLocation, C.POINTER(C.c_void_p), C.POINTER(C.c_uint), C.POINTER(C.c_uint), C.POINTER(C.c_uint)]),
        "clang_getFileName": (CXString, [C.c_void_p]),
        "clang_getCursorReferenced": (CXCursor, [CXCursor]),
        "clang_Cursor_getStorageClass": (C.c_int, [CXCursor]),
        "clang_getCursorSemanticParent": (CXCursor, [CXCursor]),
        "clang_getCursorBinaryOperatorKind": (C.c_int, [CXCursor]),
        "clang_getCursorUnaryOperatorKind": (C.c_int, [CXCursor]),
        "clang_getCursorSpelling": (CXString, [CXCursor]),
        "clang_getCursorExtent": (CXSourceRange, [CXCursor]),
        "clang_Cursor_getTranslationUnit": (C.c_void_p, [CXCursor]),
        "clang_tokenize": (None, [C.c_void_p, CXSourceRange, C.POINTER(C.POINTER(CXToken)), C.POINTER(C.c_uint)]),
        "clang_getTokenSpelling": (CXString, [C.c_void_p, CXToken]),
        "clang_disposeTokens": (None, [C.c_void_p, C.POINTER(CXToken), C.c_uint]),
        "clang_Cursor_isNull": (C.c_int, [CXCursor]),
        "clang_equalCursors": (C.c_uint, [CXCursor, CXCursor]),
        "clang_getNumDiagnostics": (C.c_uint, [C.c_void_p]),
        "clang_getRangeStart": (CXSourceLocation, [CXSourceRange]),
        "clang_getRangeEnd": (CXSourceLocation, [CXSourceRange]),
        "clang_getFileLocation": (None, [CXSourceLocation, C.POINTER(C.c_void_p), C.POINTER(C.c_uint), C.POINTER(C.c_uint), C.POINTER(C.c_uint)]),
    }
    for name, (res, args) in sig.items():
        f = getattr(lib, name)
        f.restype = res
        f.argtypes = args
    return lib

L = None
def s(cx):
    r = L.clang_getCString(cx)
    v = r.decode("utf-8", "replace") if r else ""
    L.clang_disposeString(cx)
    return v

# cursor kinds
K_STRUCT, K_UNION, K_FIELD, K_FUNC, K_VAR, K_PARM, K_TYPEDEF = 2, 3, 6, 8, 9, 10, 20
K_UNEXPOSED, K_DECLREF, K_MEMBERREF, K_CALL = 100, 101, 102, 103
K_PAREN, K_UNARY, K_SUBSCRIPT, K_BINARY, K_COMPOUND_ASSIGN, K_CAST = 111, 112, 113, 114, 115, 117
K_COMPOUND_STMT = 202
K_MACRO_EXPANSION = 502
# type kinds
T_POINTER, T_RECORD, T_ENUM, T_TYPEDEF = 101, 105, 106, 107
T_CONST_ARRAY, T_INCOMPLETE_ARRAY, T_VAR_ARRAY, T_ELABORATED = 112, 114, 115, 119
BO_ASSIGN = 22
UO_DEREF = 6

REF_TYPEDEFS = {"GCHandle", "GCValue", "GCValueConst"}
NON_REF_TYPEDEFS = {"JSAtom"}   # atoms are permanent: never collected

class Analyzer:
    def __init__(self):
        self.ref_cache = {}

    def strip(self, t):
        while t.kind == T_ELABORATED:
            t = L.clang_Type_getNamedType(t)
        return t

    def is_ref(self, t, depth=0):
        t = self.strip(t)
        if depth > 12:
            return False
        if t.kind == T_TYPEDEF:
            name = s(L.clang_getTypeSpelling(t)).replace("const ", "").replace("volatile ", "").strip()
            if name in REF_TYPEDEFS: return True
            if name in NON_REF_TYPEDEFS: return False
            key = "td:" + name
            if key in self.ref_cache: return self.ref_cache[key]
            self.ref_cache[key] = False
            decl = L.clang_getTypeDeclaration(t)
            v = self.is_ref(L.clang_getTypedefDeclUnderlyingType(decl), depth + 1)
            self.ref_cache[key] = v
            return v
        if t.kind in (T_CONST_ARRAY, T_INCOMPLETE_ARRAY, T_VAR_ARRAY):
            return self.is_ref(L.clang_getArrayElementType(t), depth + 1)
        if t.kind == T_RECORD:
            name = s(L.clang_getTypeSpelling(L.clang_getCanonicalType(t)))
            key = "rec:" + name
            if key in self.ref_cache: return self.ref_cache[key]
            self.ref_cache[key] = False
            found = [False]
            def fv(cur, _):
                if self.is_ref(L.clang_getCursorType(cur), depth + 1):
                    found[0] = True
                    return 0  # break
                return 1      # continue
            L.clang_Type_visitFields(t, FIELD_VISITOR(fv), None)
            self.ref_cache[key] = found[0]
            return found[0]
        return False

def children(c):
    out = []
    def v(cur, parent, _):
        out.append(cur)
        return 1
    L.clang_visitChildren(c, VISITOR(v), None)
    return out

def loc_of(c, expansion=True):
    f = C.c_void_p(); line = C.c_uint(); col = C.c_uint(); off = C.c_uint()
    loc = L.clang_getCursorLocation(c)
    (L.clang_getExpansionLocation if expansion else L.clang_getSpellingLocation)(loc, C.byref(f), C.byref(line), C.byref(col), C.byref(off))
    name = s(L.clang_getFileName(f)) if f.value else ""
    return name, line.value, col.value, off.value

def tokens(c):
    tu = L.clang_Cursor_getTranslationUnit(c)
    rng = L.clang_getCursorExtent(c)
    toks = C.POINTER(CXToken)(); n = C.c_uint()
    L.clang_tokenize(tu, rng, C.byref(toks), C.byref(n))
    out = [s(L.clang_getTokenSpelling(tu, toks[i])) for i in range(n.value)]
    L.clang_disposeTokens(tu, toks, n)
    return out

def strip_expr(c):
    while True:
        k = L.clang_getCursorKind(c)
        if k in (K_PAREN, K_UNEXPOSED, K_CAST):
            ch = children(c)
            if k == K_CAST:
                ch = [x for x in ch if L.clang_getCursorKind(x) >= 100 and L.clang_getCursorKind(x) < 200]
            if len(ch) != 1:
                return c
            c = ch[0]
            continue
        return c

def is_local_var(decl):
    k = L.clang_getCursorKind(decl)
    if k == K_PARM: return True
    if k != K_VAR: return False
    sc = L.clang_Cursor_getStorageClass(decl)  # 1 none, 2 extern, 3 static, ...
    parent = L.clang_getCursorSemanticParent(decl)
    return L.clang_getCursorKind(parent) == K_FUNC and sc not in (2, 3)

def lvalue_may_be_heap(c):
    """False when the lvalue is a local variable (or a field/element of one)."""
    c = strip_expr(c)
    k = L.clang_getCursorKind(c)
    if k == K_DECLREF:
        decl = L.clang_getCursorReferenced(c)
        if L.clang_Cursor_isNull(decl): return True
        if is_local_var(decl): return False
        return False   # globals: off-heap (roots are registered separately)
    if k == K_MEMBERREF:
        ch = children(c)
        if not ch: return False   # implicit this / anonymous member
        base = ch[0]
        bt = A.strip(L.clang_getCursorType(base))
        if bt.kind == T_POINTER or (bt.kind == T_TYPEDEF and A.strip(L.clang_getCanonicalType(bt)).kind == T_POINTER):
            return True
        return lvalue_may_be_heap(base)
    if k == K_SUBSCRIPT:
        ch = children(c)
        if not ch: return True
        base = strip_expr(ch[0])
        bt = L.clang_getCanonicalType(L.clang_getCursorType(base))
        if bt.kind in (T_CONST_ARRAY, T_INCOMPLETE_ARRAY, T_VAR_ARRAY):
            return lvalue_may_be_heap(base)
        return True
    if k == K_UNARY:
        return True
    return True

DIRTY_CALLS = re.compile(r"\b(gc_dirty_slot|gc_dirty_range|gc_dirty_object|GC_WRITE_BARRIER\w*|gc_write_barrier\w*|GC_DIRTY\w*|set_value|gc_list_\w+)\b")
# memset only clears memory, which cannot hide a reference.
# Write-helper macros that mark the slot dirty themselves.
HELPER_MACROS = re.compile(r"\b(SAFE_SET|SAFE_SET_HANDLE|SAFE_SET_VALUE|GC_SET)\s*\(")
STORE_CALLS = {"memcpy", "memmove", "__builtin_memcpy", "__builtin_memmove"}

# Right-hand sides that never produce a reference: overwriting a slot with
# one of these can only remove an edge, which concurrent marking tolerates.
NON_REF_RHS = re.compile(r"^\(?\s*(JS_NULL|JS_UNDEFINED|JS_FALSE|JS_TRUE|JS_EXCEPTION|JS_UNINITIALIZED|"
                         r"GC_UNDEFINED|GC_NULL|GC_HANDLE_NULL|GC_HANDLE_FREED|NULL|0|"
                         r"JS_NewInt32|JS_NewBool|JS_NewFloat64|__JS_NewFloat64|JS_NewInt64|JS_NewUint32|"
                         r"JS_MKVAL|JS_NewCatchOffset|js_int32|js_float64|js_bool|JS_NAN|JS_NewNumber)\b")
ATOMIC_STORE_CALLS = {"atomic_store", "atomic_store_explicit", "atomic_exchange", "atomic_exchange_explicit",
                      "atomic_compare_exchange_strong", "atomic_compare_exchange_weak",
                      "atomic_compare_exchange_strong_explicit", "atomic_compare_exchange_weak_explicit",
                      "__atomic_store_n", "__atomic_exchange_n", "__atomic_compare_exchange_n",
                      # engine helpers (quickjs_gc_unified.h)
                      "atomic_store_u32", "atomic_store_u64", "atomic_store_128", "atomic_store_ptr",
                      "atomic_compare_exchange_u32", "atomic_compare_exchange_u64", "atomic_exchange_u32"}

# The interpreter's operand stack and the buffers of the frame being run.
# Frames on the runtime's frame chain are rescanned in full when marking
# finishes, so stores into them need no dirty flag; a frame that leaves the
# chain (a generator or async function suspending) is marked dirty as a whole.
FRAME_ROOTS = {"sp"}
FRAME_FUNCS = {"JS_CallInternal": {"sp", "arg_buf", "var_buf", "stack_buf"}}

def is_active_frame_slot(root, func_name):
    if root in FRAME_ROOTS: return True
    return root in FRAME_FUNCS.get(func_name, ())

def call_name(c):
    return s(L.clang_getCursorSpelling(c))

def addr_of_local_or_global(arg):
    a = strip_expr(arg)
    if L.clang_getCursorKind(a) == K_UNARY and tokens(a)[:1] == ["&"]:
        ch = [x for x in children(a) if 100 <= L.clang_getCursorKind(x) < 200]
        return len(ch) == 1 and not lvalue_may_be_heap(ch[0])
    if L.clang_getCursorKind(a) == K_DECLREF:
        decl = L.clang_getCursorReferenced(a)
        t = L.clang_getCanonicalType(L.clang_getCursorType(decl))
        return t.kind in (T_CONST_ARRAY, T_INCOMPLETE_ARRAY)   # array variable
    return False

def pointee_ref(arg):
    """For memcpy-like and atomic-store destinations: does the pointer, with
    casts stripped, point at a reference type?"""
    if addr_of_local_or_global(arg): return False
    a = arg
    best = None
    for _ in range(8):
        t = L.clang_getCursorType(a)
        ct = L.clang_getCanonicalType(t)
        if ct.kind == T_POINTER:
            best = L.clang_getPointeeType(t)
            if A.is_ref(best): return True
        elif ct.kind in (T_CONST_ARRAY, T_INCOMPLETE_ARRAY):
            if A.is_ref(L.clang_getArrayElementType(ct)): return True
        k = L.clang_getCursorKind(a)
        if k == K_UNARY and tokens(a)[:1] == ["&"]:
            ch = [x for x in children(a) if 100 <= L.clang_getCursorKind(x) < 200]
            return len(ch) == 1 and A.is_ref(L.clang_getCursorType(ch[0])) and lvalue_may_be_heap(ch[0])
        ch = [x for x in children(a) if 100 <= L.clang_getCursorKind(x) < 200]
        if k not in (K_PAREN, K_UNEXPOSED, K_CAST) or len(ch) != 1:
            return False
        a = ch[0]
    return False

class Site:
    __slots__ = ("file", "line", "col", "func", "kind", "text", "macro", "covered", "lhs_root",
                 "begin", "end", "lhs_begin", "lhs_end", "rhs_begin", "rhs_end")

def extent_offsets(c):
    rng = L.clang_getCursorExtent(c)
    out = []
    for loc in (L.clang_getRangeStart(rng), L.clang_getRangeEnd(rng)):
        f = C.c_void_p(); line = C.c_uint(); col = C.c_uint(); off = C.c_uint()
        L.clang_getFileLocation(loc, C.byref(f), C.byref(line), C.byref(col), C.byref(off))
        out.append(off.value)
    return out

TYPE_WORDS = {"volatile", "const", "struct", "union", "unsigned", "signed", "int", "char", "void", "long", "short",
              "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int32_t", "int64_t", "uintptr_t", "GCValue", "GCHandle"}

def lhs_root_text(toks):
    """Leading identifier of the lvalue, past casts and unary operators,
    used to match a dirty call that follows."""
    for t in toks:
        if re.match(r"[A-Za-z_]\w*$", t) and t not in TYPE_WORDS and not t.endswith("Handle"):
            return t
    return ""

A = None

def analyze(tu_cursor, project_root, results):
    stack_funcs = []

    def visit_block(block_cursor, func_name):
        stmts = children(block_cursor)
        for i, st in enumerate(stmts):
            later = stmts[i + 1:i + 4]
            scan(st, func_name, later)

    def covered_by_following(later, root):
        for st in later:
            txt = " ".join(tokens(st))
            if DIRTY_CALLS.search(txt) and (not root or root in txt):
                return True
        return False

    def report(c, func_name, kind, toks, later, root, lhs=None, rhs=None):
        fname, line, col, _ = loc_of(c, expansion=True)
        sname, sline, _, _ = loc_of(c, expansion=False)
        if not fname.startswith(project_root): return
        if os.path.basename(fname) in COLLECTOR_FILES: return
        site = Site()
        site.file, site.line, site.col, site.func, site.kind = fname, line, col, func_name, kind
        site.text = " ".join(toks)[:160]
        site.macro = (sname != fname or sline != line)
        site.lhs_root = root
        site.begin, site.end = extent_offsets(c)
        site.lhs_begin, site.lhs_end = extent_offsets(lhs) if lhs is not None else (0, 0)
        site.rhs_begin, site.rhs_end = extent_offsets(rhs) if rhs is not None else (0, 0)
        if "gc-no-dirty" in open_line(fname, line):
            site.covered = True
            results.append(site)
            return
        if not toks:
            site.macro = True
            site.text = open_line(fname, line).strip()[:160]
            if HELPER_MACROS.search(site.text) or macro_body_uses_helper(fname, site.text):
                site.covered = True
                results.append(site)
                return
        site.covered = covered_by_following(later, root)
        if site.macro:
            # Inside a macro: covered if the macro body marks the slot dirty.
            site.covered = site.covered or bool(DIRTY_CALLS.search(open_line(sname, sline, 8)))
        results.append(site)

    def scan(c, func_name, later):
        k = L.clang_getCursorKind(c)
        if k == K_COMPOUND_STMT:
            visit_block(c, func_name)
            return
        if k == K_BINARY and L.clang_getCursorBinaryOperatorKind(c) == BO_ASSIGN:
            ch = children(c)
            if len(ch) == 2 and A.is_ref(L.clang_getCursorType(ch[0])) and lvalue_may_be_heap(ch[0]):
                toks = tokens(c)
                if toks:
                    rhs_text = " ".join(toks[toks.index("=") + 1:]) if "=" in toks else ""
                else:
                    fname, line, _, _ = loc_of(c)
                    src = open_line(fname, line)
                    rhs_text = src.split("=", 1)[1].strip() if "=" in src else ""
                lt = tokens(ch[0])
                root = lhs_root_text(lt)
                if not NON_REF_RHS.match(rhs_text) and not is_active_frame_slot(root, func_name):
                    report(c, func_name, "assign", toks, later, root, ch[0], ch[1])
        elif k == K_CALL:
            name = call_name(c)
            if name in STORE_CALLS or name in ATOMIC_STORE_CALLS:
                args = [x for x in children(c) if 100 <= L.clang_getCursorKind(x) < 200][1:] if False else None
                ch = children(c)
                exprs = [x for x in ch if 100 <= L.clang_getCursorKind(x) < 200]
                # first child is the callee reference
                if len(exprs) >= 2 and pointee_ref(exprs[1]):
                    at = tokens(exprs[1])
                    report(c, func_name, name, tokens(c), later, lhs_root_text(at), exprs[1], None)
        for ch in children(c):
            scan(ch, func_name, later)

    for top in children(tu_cursor):
        if L.clang_getCursorKind(top) != K_FUNC: continue
        fname, _, _, _ = loc_of(top)
        if not fname.startswith(project_root): continue
        name = s(L.clang_getCursorSpelling(top))
        for ch in children(top):
            if L.clang_getCursorKind(ch) == K_COMPOUND_STMT:
                visit_block(ch, name)

_line_cache = {}
def open_line(path, line, n=1):
    if path not in _line_cache:
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                _line_cache[path] = f.read().split("\n")
        except OSError:
            _line_cache[path] = []
    lines = _line_cache[path]
    # A multi-line macro: read until the continuation lines end.
    out = []
    i = line - 1
    while 0 <= i < len(lines) and len(out) < 40:
        out.append(lines[i])
        if not lines[i].rstrip().endswith("\\") and len(out) >= n:
            break
        i += 1
    return "\n".join(out)

STMT_BEFORE = re.compile(r"(?:[{};:)]|\belse|^)\s*$", re.S)

def apply_fixes(sites):
    """Rewrite simple statements in place:
         LHS = RHS;           ->  GC_SET(LHS, RHS);
         memcpy(d, s, n)      ->  gc_memcpy(d, s, n)
       Sites inside macros or nested in larger expressions are left alone
       and listed for manual review."""
    by_file = {}
    for r in sites:
        by_file.setdefault(r.file, []).append(r)
    manual = []
    for path, rs in by_file.items():
        data = open(path, "rb").read()
        edits = []
        for r in sorted(set((x.begin, x.end, x.lhs_begin, x.lhs_end, x.rhs_begin, x.rhs_end, x.kind, x.macro, x.line)
                            for x in rs)):
            b, e, lb, le, rb, re_, kind, macro, line = r
            if macro or e <= b:
                manual.append("%s:%d" % (path, line)); continue
            if kind == "assign":
                after = data[e:e + 40].lstrip()
                before = data[max(0, b - 200):b].decode("utf-8", "replace")
                if not after.startswith(b";") or not STMT_BEFORE.search(before) or not (b <= lb < le <= rb < re_ <= e):
                    manual.append("%s:%d" % (path, line)); continue
                lhs = data[lb:le].decode()
                rhs = data[rb:re_].decode()
                if "\n" in lhs or rhs.count("(") != rhs.count(")"):
                    manual.append("%s:%d" % (path, line)); continue
                edits.append((b, e, ("GC_SET(%s, %s)" % (lhs, rhs)).encode()))
            elif kind in ("memcpy", "memmove", "__builtin_memcpy", "__builtin_memmove"):
                name = kind.replace("__builtin_", "")
                if data[b:b + len(kind)].decode() != kind:
                    manual.append("%s:%d" % (path, line)); continue
                edits.append((b, b + len(kind), ("gc_" + name).encode()))
            else:
                manual.append("%s:%d" % (path, line))
        edits.sort(key=lambda x: -x[0])
        last = None
        for b, e, rep in edits:
            if last is not None and e > last:   # overlapping (nested) edit: skip
                continue
            data = data[:b] + rep + data[e:]
            last = b
        open(path, "wb").write(data)
        print("fixed %d sites in %s" % (len(edits), os.path.relpath(path)))
    for m in sorted(set(manual)):
        print("manual:", os.path.relpath(m))

# The collector's own files manage off-heap structures (worklists, rings,
# registries) and are not checked.
COLLECTOR_FILES = {"quickjs_gc_unified.c", "gc_collector.c", "gc_concurrent.c", "gc_platform.c",
                   "job_pool.c", "lockfree_hash_table.c"}

def macro_body_uses_helper(path, text):
    """The expansion line calls a macro (defined in the same file) whose
    body stores only through the write-helper macros."""
    m = re.match(r"([A-Za-z_]\w*)\s*\(", text)
    if not m: return False
    if path not in _line_cache: open_line(path, 1)
    lines = _line_cache.get(path, [])
    for i, l in enumerate(lines):
        if re.match(r"\s*#\s*define\s+%s\b" % re.escape(m.group(1)), l):
            body = open_line(path, i + 1, 1)
            return bool(HELPER_MACROS.search(body))
    return False

def main(argv):
    global L, A
    lib = None
    listing = False
    args = []
    i = 0
    while i < len(argv):
        if argv[i] == "--lib": lib = argv[i + 1]; i += 2; continue
        if argv[i] == "--list": listing = True; i += 1; continue
        args.append(argv[i]); i += 1
    fix = "--fix" in args
    args = [a for a in args if a != "--fix"]
    if not args:
        print(__doc__); return 2
    for cand in ([lib] if lib else LIB_CANDIDATES):
        if cand and os.path.exists(cand):
            L = load(cand); break
    if L is None:
        print("libclang not found; pass --lib"); return 2
    A = Analyzer()
    db = json.load(open(args[0]))
    wanted = [os.path.abspath(f) for f in args[1:]]
    project_root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))
    idx = L.clang_createIndex(0, 0)
    results = []
    for entry in db:
        f = os.path.abspath(os.path.join(entry["directory"], entry["file"]))
        if wanted and f not in wanted: continue
        if not f.startswith(project_root) or not f.endswith(".c"): continue
        if "/mbedtls/" in f or "/stb/" in f or "/nanosvg/" in f or "/minimp4/" in f: continue
        if os.path.basename(f) in COLLECTOR_FILES: continue
        cmd = entry.get("arguments") or entry["command"].split()
        flags = [a for a in cmd[1:] if a.startswith(("-I", "-D", "-std", "-include"))]
        cargs = (C.c_char_p * len(flags))(*[a.encode() for a in flags])
        tu = L.clang_parseTranslationUnit(idx, f.encode(), cargs, len(flags), None, 0, 0)
        if not tu:
            print("parse failed:", f); continue
        analyze(L.clang_getTranslationUnitCursor(tu), project_root, results)
    seen = set()
    uncovered = 0
    by_file = {}
    for r in results:
        key = (r.file, r.line, r.col, r.kind)
        if key in seen: continue
        seen.add(key)
        by_file.setdefault(r.file, [0, 0])
        by_file[r.file][0] += 1
        if not r.covered:
            by_file[r.file][1] += 1
            uncovered += 1
            if listing:
                print("%s:%d:%d: %s [%s]%s %s" % (os.path.relpath(r.file), r.line, r.col, r.func, r.kind,
                                               " (macro)" if r.macro else "", r.text))
    if fix:
        apply_fixes([r for r in results if not r.covered])
    for f, (n, u) in sorted(by_file.items(), key=lambda x: -x[1][1]):
        print("%6d stores, %6d uncovered  %s" % (n, u, os.path.relpath(f)))
    print("total uncovered:", uncovered)
    return 1 if uncovered else 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
