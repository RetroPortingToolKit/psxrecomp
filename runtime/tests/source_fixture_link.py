"""Compile production modules for focused fixtures; unexpected link seams abort."""
import os
import re
import subprocess
from pathlib import Path

SYMBOL = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
MULTIPLE = re.compile(r"multiple definition of [`']([^`']+)[`']")
UNDEFINED = re.compile(r"undefined reference to [`']([^`']+)[`']")

# A definition in our stub object would silently override a symbol that the C
# library would otherwise satisfy from an archive/shared object, so anything
# libc/CRT/compiler-runtime provides must never be stubbed.
LIBRARY_CANDIDATES = ["libc.a", "libm.a", "libgcc.a", "libmingw32.a",
                      "libmingwex.a", "libmsvcrt.a", "libucrt.a",
                      "libkernel32.a"]
ISO_C_FALLBACK = {
    "abort", "atexit", "atoi", "atol", "calloc", "exit", "fclose", "ferror",
    "fflush", "fopen", "fprintf", "fputc", "fputs", "fread", "free", "fwrite",
    "getenv", "longjmp", "malloc", "memcpy", "memmove", "memset", "printf",
    "putchar", "puts", "qsort", "realloc", "setjmp", "snprintf", "sprintf",
    "sscanf", "strcat", "strchr", "strcmp", "strcpy", "strlen", "strncat",
    "strncmp", "strncpy", "strstr", "strtol", "strtoll", "strtoul",
    "strtoull", "time", "vfprintf", "vsnprintf",
    # glibc's setjmp() is a macro for _setjmp. Without a static libc.a to list
    # (Rocky 9), a stub of that name aborted every fixture that links a
    # setjmp user: traps.c, interrupts.c.
    "_setjmp", "stdin", "stdout", "stderr",
}


def library_symbols(cc):
    provided = set(ISO_C_FALLBACK)
    for name in LIBRARY_CANDIDATES:
        out = subprocess.run([cc, "-print-file-name=" + name],
                             capture_output=True, text=True, encoding="utf-8", errors="replace").stdout.strip()
        if not out or not os.path.exists(out):
            continue
        listing = subprocess.run(["nm", out], capture_output=True, text=True, encoding="utf-8", errors="replace").stdout
        for line in listing.splitlines():
            parts = line.split()
            if len(parts) >= 2 and parts[-2] != "U" and SYMBOL.match(parts[-1]):
                provided.add(parts[-1])
    return provided



def nm_symbols(objs):
    defined, undefined = set(), set()
    for obj in objs:
        out = subprocess.run(["nm", str(obj)], capture_output=True, text=True, encoding="utf-8", errors="replace")
        for line in out.stdout.splitlines():
            parts = line.split()
            if len(parts) < 2:
                continue
            sym, typ = parts[-1], parts[-2]
            if not SYMBOL.match(sym):
                continue
            (undefined if typ == "U" else defined).add(sym)
    return defined, undefined


def write_stubs(path, symbols):
    lines = ["#include <stdlib.h>", "/* Unrelated link seams must not execute. */"]
    lines += ["void %s(void) { abort(); }" % s for s in sorted(symbols)]
    path.write_text("\n".join(lines) + "\n")


def build_and_run(cc, here, src_root, opt, work, modules, test_name, defines=()):
    tag = opt.replace("-", "")
    include = str(src_root / "include")
    objs = []
    for src in modules:
        obj = work / (Path(src).stem + tag + ".o")
        subprocess.run([cc, "-std=c++17" if Path(src).suffix == ".cpp" else "-std=c11", opt, "-w", "-I", include,
                        *["-D" + d for d in defines],
                        "-c", str(src_root / "src" / src), "-o", str(obj)],
                       check=True)
        objs.append(obj)

    defined, undefined = nm_symbols(objs)
    provided = library_symbols(cc)
    stubs = {s for s in undefined - defined - provided if not s.startswith("__")}
    stub_c = work / ("stubs" + tag + ".c")
    stub_o = work / ("stubs" + tag + ".o")

    test_c = here / test_name
    test_o = work / ("sectionwire" + tag + ".o")
    subprocess.run([cc, "-std=c11", opt, "-Wall", "-Wextra", "-Werror",
                    "-I", include, "-c", str(test_c), "-o", str(test_o)],
                   check=True)
    exe = work / ("sectionwire" + tag + (".exe" if os.name == "nt" else ""))

    for _ in range(40):
        write_stubs(stub_c, stubs)
        subprocess.run([cc, "-O0", "-w", "-c", str(stub_c), "-o", str(stub_o)],
                       check=True)
        result = subprocess.run(
            [cc, str(test_o)] + [str(o) for o in objs] + [str(stub_o),
             "-o", str(exe)],
            capture_output=True, text=True, encoding="utf-8", errors="replace")
        if result.returncode == 0:
            break
        errors = result.stderr
        changed = False
        for sym in MULTIPLE.findall(errors):
            if sym in stubs:
                stubs.discard(sym)
                changed = True
        for sym in UNDEFINED.findall(errors):
            if sym not in defined and sym not in provided and sym not in stubs:
                stubs.add(sym)
                changed = True
        if not changed:
            raise SystemExit("link did not converge for %s:\n%s" % (opt, errors))
    else:
        raise SystemExit("link did not converge for %s" % opt)

    env = {k: v for k, v in os.environ.items() if not k.upper().startswith("PSX_")}
    run = subprocess.run([str(exe)], env=env, capture_output=True, text=True, encoding="utf-8", errors="replace",
                         timeout=60)
    if run.returncode != 0:
        raise SystemExit("%s: runtime fixture failed (rc=%d)\n%s%s"
                         % (opt, run.returncode, run.stdout, run.stderr))
