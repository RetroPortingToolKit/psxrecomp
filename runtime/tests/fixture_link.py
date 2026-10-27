"""Link production runtime modules into a focused C fixture.

build_and_run() compiles the named runtime/src modules and the fixture, then
links them. Every symbol the modules need but neither they, the fixture nor
the C library define becomes a stub that calls abort(), so a fixture only
defines the seams its path really uses and an unexpected call fails loudly.
GNU toolchains only (nm, GNU ld diagnostics).
"""
import os
import re
import subprocess
from pathlib import Path

SYMBOL = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
MULTIPLE = re.compile(r"multiple definition of [`']([^`']+)[`']")
UNDEFINED = re.compile(r"undefined reference to [`']([^`']+)[`']")

# A stub would silently override a symbol the C library would otherwise
# provide from an archive or shared object, so anything libc, the CRT or the
# compiler runtime defines is never stubbed.
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
}


def _run(args, **kwargs):
    return subprocess.run(args, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", **kwargs)


def library_symbols(cc):
    provided = set(ISO_C_FALLBACK)
    for name in LIBRARY_CANDIDATES:
        out = _run([cc, "-print-file-name=" + name]).stdout.strip()
        if not out or not os.path.exists(out):
            continue
        for line in _run(["nm", out]).stdout.splitlines():
            parts = line.split()
            if len(parts) >= 2 and parts[-2] != "U" and SYMBOL.match(parts[-1]):
                provided.add(parts[-1])
    return provided


def nm_symbols(objs):
    defined, undefined = set(), set()
    for obj in objs:
        for line in _run(["nm", str(obj)]).stdout.splitlines():
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
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def build_and_run(cc, here, src_root, opt, work, modules, test_name, defines=()):
    """Compile runtime/src `modules` and `test_name` at `opt`, link, run."""
    tag = opt.replace("-", "")
    include = str(src_root / "include")
    objs = []
    for src in modules:
        obj = work / (Path(src).stem + tag + ".o")
        subprocess.run([cc, "-std=c11", opt, "-w", "-I", include,
                        *["-D" + d for d in defines],
                        "-c", str(src_root / "src" / src), "-o", str(obj)],
                       check=True)
        objs.append(obj)

    stub_c = work / ("stubs" + tag + ".c")
    stub_o = work / ("stubs" + tag + ".o")

    test_o = work / ("fixture" + tag + ".o")
    subprocess.run([cc, "-std=c11", opt, "-Wall", "-Wextra", "-Werror",
                    "-I", include, "-c", str(here / test_name), "-o", str(test_o)],
                   check=True)
    exe = work / ("fixture" + tag + (".exe" if os.name == "nt" else ""))
    # Include fixture seams before synthesizing stubs. Besides avoiding a
    # needless duplicate-symbol link, this works with LLVM's diagnostics too.
    defined, undefined = nm_symbols(objs + [test_o])
    provided = library_symbols(cc)
    stubs = {s for s in undefined - defined - provided if not s.startswith("__")}

    for _ in range(40):
        write_stubs(stub_c, stubs)
        subprocess.run([cc, "-O0", "-w", "-c", str(stub_c), "-o", str(stub_o)],
                       check=True)
        result = _run([cc, str(test_o)] + [str(o) for o in objs] +
                      [str(stub_o), "-o", str(exe)])
        if result.returncode == 0:
            break
        changed = False
        for sym in MULTIPLE.findall(result.stderr):
            if sym in stubs:
                stubs.discard(sym)
                changed = True
        for sym in UNDEFINED.findall(result.stderr):
            if sym not in defined and sym not in provided and sym not in stubs:
                stubs.add(sym)
                changed = True
        if not changed:
            raise SystemExit("link did not converge for %s:\n%s" % (opt, result.stderr))
    else:
        raise SystemExit("link did not converge for %s" % opt)

    env = {k: v for k, v in os.environ.items() if not k.upper().startswith("PSX_")}
    run = _run([str(exe)], env=env, timeout=60)
    if run.returncode != 0:
        raise SystemExit("%s: fixture failed (rc=%d)\n%s%s"
                         % (opt, run.returncode, run.stdout, run.stderr))
