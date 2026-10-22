"""Link production runtime modules into a focused C fixture.

build_and_run() compiles the named runtime/src modules and the fixture, then
links them. Every symbol the modules need but neither they, the fixture nor
the C library define becomes a stub that calls abort(), so a fixture only
defines the seams its path really uses and an unexpected call fails loudly.
GNU and Apple toolchains (nm and their linker diagnostics).
"""
import os
import re
import subprocess
import sys
from pathlib import Path

SYMBOL = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
MULTIPLE = re.compile(r"multiple definition of [`']([^`']+)[`']")
UNDEFINED = re.compile(r"undefined reference to [`']([^`']+)[`']")
APPLE_MULTIPLE = re.compile(r"duplicate symbol '([^']+)'")
APPLE_UNDEFINED = re.compile(r'"(_[A-Za-z_][A-Za-z0-9_]*)", referenced from:')


def source_symbol(symbol):
    return symbol[1:] if sys.platform == 'darwin' and symbol.startswith('_') else symbol

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
ISO_C_FALLBACK.update({
    'sin', 'sinf', 'cos', 'cosf', 'tan', 'tanf', 'sqrt', 'sqrtf', 'pow', 'powf',
    'floor', 'floorf', 'ceil', 'ceilf', 'round', 'roundf', 'lround', 'lroundf',
    'lrint', 'lrintf', 'fmod', 'fmodf', 'fabs', 'fabsf', 'fmin', 'fminf',
    'fmax', 'fmaxf', 'ldexp', 'ldexpf', 'exp', 'expf', 'log', 'logf',
})


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
                provided.add(source_symbol(parts[-1]))
    return provided


def nm_symbols(objs):
    defined, undefined = set(), set()
    for obj in objs:
        for line in _run(["nm", str(obj)]).stdout.splitlines():
            parts = line.split()
            if len(parts) < 2:
                continue
            sym, typ = source_symbol(parts[-1]), parts[-2]
            if not SYMBOL.match(sym):
                continue
            (undefined if typ == "U" else defined).add(sym)
    return defined, undefined


def write_stubs(path, symbols):
    lines = ["#include <stdlib.h>", "#include <stdio.h>",
             "/* Unrelated link seams must not execute. */"]
    lines += ['void %s(void) { fputs("unexpected fixture seam: %s\\n", stderr); abort(); }'
              % (s, s) for s in sorted(symbols)]
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

    defined, undefined = nm_symbols(objs)
    provided = library_symbols(cc)
    stubs = {s for s in undefined - defined - provided if not s.startswith("__")}
    stub_c = work / ("stubs" + tag + ".c")
    stub_o = work / ("stubs" + tag + ".o")

    test_o = work / ("fixture" + tag + ".o")
    subprocess.run([cc, "-std=c11", opt, "-Wall", "-Wextra", "-Werror",
                    "-I", include, "-c", str(here / test_name), "-o", str(test_o)],
                   check=True)
    # On Apple toolchains a tentative fixture global can be a common symbol.
    # A strong function stub would override it without a duplicate-symbol
    # error. Exclude fixture definitions before the first link on every host.
    fixture_defined, _ = nm_symbols([test_o])
    stubs.difference_update(fixture_defined)
    exe = work / ("fixture" + tag + (".exe" if os.name == "nt" else ""))

    for _ in range(40):
        write_stubs(stub_c, stubs)
        subprocess.run([cc, "-O0", "-w", "-c", str(stub_c), "-o", str(stub_o)],
                       check=True)
        result = _run([cc, str(test_o)] + [str(o) for o in objs] +
                      [str(stub_o), "-o", str(exe)])
        if result.returncode == 0:
            break
        changed = False
        multiple = MULTIPLE.findall(result.stderr)
        multiple += [source_symbol(s) for s in APPLE_MULTIPLE.findall(result.stderr)]
        for sym in multiple:
            if sym in stubs:
                stubs.discard(sym)
                changed = True
        undefined = UNDEFINED.findall(result.stderr)
        undefined += [source_symbol(s) for s in APPLE_UNDEFINED.findall(result.stderr)]
        for sym in undefined:
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
