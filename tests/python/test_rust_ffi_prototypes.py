# SPDX-License-Identifier: GPL-2.0-only
"""Check the C prototypes of the Rust exports against their Rust definitions.

The C side declares each `#[no_mangle] extern "C"` Rust function by hand. Nothing
links the two, so a prototype with too few arguments still compiles and the Rust
side then reads whatever happens to be in the missing argument registers.
"""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]

RUST_EXPORT = re.compile(
    r'#\[no_mangle\]\s*pub\s+(?:unsafe\s+)?extern\s+"C"\s+fn\s+(\w+)\s*\(([^)]*)\)'
    r'\s*(?:->\s*([^{]+?))?\s*\{')

SCALARS = {
    "usize": "size", "c_size_t": "size", "size_t": "size",
    "u8": "u8", "uint8_t": "u8",
    "u32": "u32", "uint32_t": "u32",
    "i32": "i32", "int32_t": "i32", "c_int": "i32", "int": "i32",
    "u64": "u64", "uint64_t": "u64",
    "i64": "i64", "int64_t": "i64",
    "bool": "bool",
    "void": "void", "()": "void",
}

C_KEYWORDS = {"return", "else", "case", "goto", "sizeof"}


def split_args(text):
    text = " ".join(text.split())
    if not text or text == "void":
        return []
    return [arg.strip() for arg in text.split(",") if arg.strip()]


def rust_kind(ty):
    ty = ty.strip()
    depth = 0
    while ty.startswith("*"):
        ty = re.sub(r"^\*\s*(?:mut|const)\s+", "", ty)
        depth += 1
    if depth:
        return f"ptr{depth}"
    return SCALARS.get(ty.split("::")[-1], ty)


def c_kind(decl, has_name):
    decl = decl.strip()
    depth = decl.count("*")
    if re.search(r"\[[^\]]*\]\s*$", decl):
        depth += 1
        decl = re.sub(r"\[[^\]]*\]\s*$", "", decl)
    if depth:
        return f"ptr{depth}"
    words = [w for w in re.findall(r"\w+", decl) if w not in ("const", "volatile", "extern")]
    if has_name and len(words) > 1:
        words = words[:-1]
    return SCALARS.get(" ".join(words), " ".join(words))


def rust_exports():
    exports = {}
    for path in sorted((ROOT / "rust/src").rglob("*.rs")):
        for name, args, ret in RUST_EXPORT.findall(path.read_text()):
            kinds = [rust_kind(arg.split(":", 1)[1]) for arg in split_args(args)]
            sig = (tuple(kinds), rust_kind(ret) if ret else "void")
            exports.setdefault(name, set()).add((sig, f"{path.relative_to(ROOT)}"))
    return exports


def c_prototypes(names):
    pattern = re.compile(
        r"^[ \t]*(?:extern[ \t]+)?([A-Za-z_][\w \t\*]*?[ \t\*])(" + "|".join(names) +
        r")[ \t]*\(([^;{}()]*)\)[ \t]*;", re.M)
    found = []
    for path in sorted((ROOT / "src").rglob("*.[ch]")):
        source = re.sub(r"/\*.*?\*/", " ", path.read_text(errors="replace"), flags=re.S)
        source = re.sub(r"//[^\n]*", " ", source)
        for ret, name, args in pattern.findall(source):
            if ret.strip() in C_KEYWORDS:
                continue
            kinds = tuple(c_kind(arg, True) for arg in split_args(args))
            found.append((name, (kinds, c_kind(ret, False)), f"{path.relative_to(ROOT)}"))
    return found


def test_c_prototypes_match_rust_exports():
    exports = rust_exports()
    assert "rust_load_image" in exports
    prototypes = c_prototypes(sorted(exports))
    seen = {name for name, _, _ in prototypes}
    assert "rust_load_image" in seen

    errors = []
    for name, sig, where in prototypes:
        rust_sigs = {s for s, _ in exports[name]}
        if sig not in rust_sigs:
            rust_where = ", ".join(sorted(w for _, w in exports[name]))
            errors.append(f"{where}: {name}{sig} does not match {rust_where}: {sorted(rust_sigs)}")
    assert not errors, "\n".join(errors)
