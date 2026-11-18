#!/usr/bin/env python3
"""Regression: the setup host names the toolchain step that failed, and keeps
PATH bounded when it activates a pack.

host/psxrecomp_codegen_host.c used to say only that the portable toolchain
"did not pass its check" whatever had failed, and the error of a new pack that
failed its check did not say why. Activation prepended the pack folders to
PATH at every call into an 8,192-character buffer, so PATH grew at each call
and a long PATH was cut off in the middle of an entry.

This test builds a probe that includes the host source and runs it in a
closed environment (every data, cache and temporary folder inside the test's
temporary folder, PATH with the system folders only). The cases:

  * activation called three times on a short PATH and on a PATH longer than
    8,192 characters: the length is the same after each call, the pack's bin
    folder is in PATH once, and every entry of the original PATH is still
    there (Windows only; elsewhere activation leaves PATH to the system);
  * a pack whose cmake does not run, one with no clang, one whose clang does
    not run, and one whose clang runs but cannot link: the note after the
    check names that step;
  * an install of a new pack whose cmake does not run: the error names it.
"""

from pathlib import Path
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parents[2]
HOST_DIR = ROOT / "host"
WINDOWS = os.name == "nt"
APPLE = sys.platform == "darwin"
EXE = ".exe" if WINDOWS else ""

PROBE = r"""
#include "psxrecomp_codegen_host.c"

/* recomp-ui symbol the host references; the toolchain code never reaches it. */
int recomp_launcher_relaunch_exe(char* o, size_t c) { (void)o; (void)c; return 0; }

int main(int argc, char** argv) {
    const char* op = argc > 1 ? argv[1] : "";
    const char* project = getenv("PROBE_PROJECT_ROOT");
#if defined(_WIN32)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
    if (project)
        snprintf(g_project_root, sizeof(g_project_root), "%s", project);
    if (strcmp(op, "activate") == 0) {
        int i;
        for (i = 0; i < 3; ++i) {
            const char* p;
            activate_toolchain_path();
            p = getenv("PATH");
            printf("len=%u\n", (unsigned)(p ? strlen(p) : 0));
        }
        printf("bin=%s\n", g_toolchain_bin);
        printf("path=%s\n", getenv("PATH") ? getenv("PATH") : "");
    } else if (strcmp(op, "heal") == 0) {
        heal_broken_toolchain_pointers();
        printf("note=%s\n", g_tc_repair_note);
    } else if (strcmp(op, "install") == 0 && argc > 2) {
        char err[600] = "";
        int ok = host_install_toolchain_from_zip(argv[2], NULL, NULL, err, sizeof(err));
        printf("ok=%d\nerr=%s\n", ok, err);
    } else {
        fprintf(stderr, "probe: unknown operation\n");
        return 2;
    }
    return 0;
}
"""

FAKES = {
    "ok": "int main(void) { return 0; }\n",
    "fail": "int main(void) { return 1; }\n",
    "version_only": ("#include <string.h>\n"
                     "int main(int c, char** v) {\n"
                     "    return (c == 2 && strcmp(v[1], \"--version\") == 0) ? 0 : 1;\n"
                     "}\n"),
}
FAKES_SH = {
    "ok": b"#!/bin/sh\nexit 0\n",
    "fail": b"#!/bin/sh\nexit 1\n",
    "version_only": b'#!/bin/sh\n[ "$#" = 1 ] && [ "$1" = "--version" ] && exit 0\nexit 1\n',
}

STEP_CMAKE = "cmake does not run"
STEP_MISSING = "clang or the linker is missing from the pack"
STEP_CLANG = "clang does not run"
STEP_LINK = "a one-line test program did not compile and link"

SANDBOX_ROOTS = {
    "LOCALAPPDATA": "localappdata",
    "APPDATA": "appdata",
    "USERPROFILE": "home",
    "HOME": "home",
    "XDG_DATA_HOME": "xdg-data",
    "RETCOMM_DATA_HOME": "data-home",
    # The first cache base whose path names "retcomm" is where the host
    # installs and looks for `latest`; this one is listed first.
    "RETCOMM_TOOLCHAIN_CACHE": "retcomm-cache",
}


def find_recomp_ui():
    env = os.environ.get("RECOMP_UI_ROOT")
    candidates = ([Path(env)] if env else []) + [ROOT.parent / "recomp-ui", ROOT / "recomp-ui"]
    for c in candidates:
        if (c / "src" / "recomp_launcher.h").is_file():
            return c
    return None


def closed_environment(sandbox):
    """The only environment the probe gets. Built from nothing."""
    env = {}
    if WINDOWS:
        system_root = os.environ.get("SystemRoot", r"C:\Windows")
        env["SystemRoot"] = system_root
        env["windir"] = system_root
        env["SystemDrive"] = os.environ.get("SystemDrive", "C:")
        env["ComSpec"] = str(Path(system_root) / "System32" / "cmd.exe")
        env["PATHEXT"] = ".COM;.EXE;.BAT;.CMD"
        env["PATH"] = os.pathsep.join([str(Path(system_root) / "System32"), system_root])
    else:
        env["PATH"] = "/usr/bin:/bin"
    for name, folder in SANDBOX_ROOTS.items():
        (sandbox / folder).mkdir(parents=True, exist_ok=True)
        env[name] = str(sandbox / folder)
    (sandbox / "temp").mkdir(parents=True, exist_ok=True)
    for name in ("TEMP", "TMP", "TMPDIR"):
        env[name] = str(sandbox / "temp")
    (sandbox / "project").mkdir(parents=True, exist_ok=True)
    env["PROBE_PROJECT_ROOT"] = str(sandbox / "project")
    return env


def make_pack(folder, programs):
    """A pack folder with bin/<name> for each (name, bytes) in programs."""
    (folder / "bin").mkdir(parents=True, exist_ok=True)
    for name, data in programs.items():
        p = folder / "bin" / (name + EXE)
        p.write_bytes(data)
        p.chmod(0o755)
    (folder / "retcomm-toolchain.json").write_text(
        json.dumps({"id": "cmake-clang-v1", "version": "1.0.20"}), encoding="utf-8")


def remove_tree(path):
    """Remove the sandbox, links as links (never what a junction points at)."""
    isjunction = getattr(os.path, "isjunction", None)
    for dirpath, dirnames, _ in os.walk(path, topdown=True):
        for d in list(dirnames):
            p = Path(dirpath) / d
            if os.path.islink(p) or (isjunction and isjunction(p)):
                if WINDOWS:
                    os.rmdir(p)
                else:
                    os.unlink(p)
                dirnames.remove(d)
    shutil.rmtree(path, ignore_errors=True)


def run(probe, sandbox, env, *args):
    return subprocess.run([str(probe)] + [str(a) for a in args], capture_output=True,
                          text=True, encoding="utf-8", errors="replace", env=env,
                          cwd=str(sandbox), timeout=120)


def build(cc, ui, tmp):
    probe_c = tmp / "probe.c"
    probe_c.write_text(PROBE, encoding="utf-8")
    probe = tmp / ("probe" + EXE)
    res = subprocess.run(
        [cc, "-std=c11", "-o", str(probe), str(probe_c),
         "-I", str(HOST_DIR), "-I", str(ROOT / "runtime" / "include"),
         "-I", str(ui / "src"), "-I", str(ui / "src" / "common")],
        capture_output=True, text=True, encoding="utf-8", errors="replace")
    if res.returncode != 0:
        print("FAIL: could not build probe\n" + res.stderr[-2000:])
        return None, None
    if not WINDOWS:
        return probe, dict(FAKES_SH)
    fakes = {}
    for name, code in FAKES.items():
        c_file = tmp / ("fake_%s.c" % name)
        c_file.write_text(code, encoding="utf-8")
        exe = tmp / ("fake_%s.exe" % name)
        res = subprocess.run([cc, "-o", str(exe), str(c_file)], capture_output=True,
                             text=True, encoding="utf-8", errors="replace")
        if res.returncode != 0:
            print("FAIL: could not build fake program %s\n%s" % (name, res.stderr[-2000:]))
            return None, None
        fakes[name] = exe.read_bytes()
    return probe, fakes


def main():
    ui = find_recomp_ui()
    if ui is None:
        print("SKIP: recomp-ui not found (set RECOMP_UI_ROOT)")
        return 0
    cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if cc is None:
        print("SKIP: no C compiler on PATH")
        return 0

    failures = 0

    def check(ok, what, detail=""):
        nonlocal failures
        if ok:
            print("ok: " + what)
        else:
            failures += 1
            print("FAIL: " + what + (("\n" + detail) if detail else ""))

    tmp = Path(tempfile.mkdtemp(prefix="tc-steps-"))
    try:
        probe, fake = build(cc, ui, tmp)
        if probe is None:
            return 1
        linker = "lld" if WINDOWS else "ld.lld"

        # 1. Activation keeps PATH bounded.
        if not WINDOWS:
            print("SKIP: activation leaves PATH to the system outside Windows")
        else:
            for label, extra in (("a short PATH", 0), ("a PATH over 8,192 characters", 9000)):
                sandbox = tmp / ("activate-%d" % extra)
                env = closed_environment(sandbox)
                pack = sandbox / "pack"
                make_pack(pack, {"cmake": fake["ok"]})
                env["RETCOMM_TOOLCHAIN_DIR"] = str(pack)
                entries = env["PATH"].split(os.pathsep)
                n = 0
                while len(os.pathsep.join(entries)) < extra:
                    entries.append(r"C:\no-such-folder\entry-%04d" % n)
                    n += 1
                env["PATH"] = os.pathsep.join(entries)
                r = run(probe, sandbox, env, "activate")
                lines = r.stdout.splitlines()
                lens = [line[4:] for line in lines if line.startswith("len=")]
                bin_dir = next((line[4:] for line in lines if line.startswith("bin=")), "")
                final = next((line[5:] for line in lines if line.startswith("path=")), "")
                final = final.split(os.pathsep)
                copies = sum(1 for e in final if bin_dir and e.lower() == bin_dir.lower())
                check(bool(bin_dir), "activation on %s finds the pack" % label,
                      r.stdout[-400:] + r.stderr[-400:])
                check(len(lens) == 3 and len(set(lens)) == 1,
                      "activation on %s leaves the PATH length the same at each call" % label,
                      "lengths: %s" % ", ".join(lens))
                check(copies == 1, "activation on %s puts the pack's bin folder in PATH once" % label,
                      "copies: %d" % copies)
                missing = [e for e in entries if e not in final]
                check(not missing, "activation on %s keeps every entry of the original PATH" % label,
                      "%d missing, first: %s" % (len(missing), missing[:1]))

        # 2. The note after a failed check names the step.
        step_cases = [
            ("cmake does not run", {"cmake": fake["fail"]}, STEP_CMAKE),
            ("clang does not run",
             {"cmake": fake["ok"], "clang": fake["fail"], linker: fake["ok"]}, STEP_CLANG),
            ("clang runs and cannot link",
             {"cmake": fake["ok"], "clang": fake["version_only"], linker: fake["ok"]}, STEP_LINK),
        ]
        if APPLE:
            print("SKIP: a pack with no clang is the normal macOS layout")
        else:
            step_cases.insert(1, ("there is no clang", {"cmake": fake["ok"]}, STEP_MISSING))
        for label, programs, step in step_cases:
            sandbox = tmp / ("heal-" + label.replace(" ", "-"))
            env = closed_environment(sandbox)
            make_pack(sandbox / SANDBOX_ROOTS["RETCOMM_TOOLCHAIN_CACHE"] / "latest", programs)
            r = run(probe, sandbox, env, "heal")
            check(("did not pass its check: %s." % step) in r.stdout,
                  "a pack where %s: the note names the step (%s)" % (label, step),
                  r.stdout.strip() + "\n" + r.stderr.strip()[-400:])

        # 3. The install error names the step of the new pack's check.
        sandbox = tmp / "install"
        env = closed_environment(sandbox)
        zip_path = sandbox / "new-pack.zip"
        with zipfile.ZipFile(zip_path, "w") as z:
            info = zipfile.ZipInfo("bin/cmake" + EXE)
            info.external_attr = (0o755 | stat.S_IFREG) << 16
            z.writestr(info, fake["fail"])
            z.writestr("retcomm-toolchain.json",
                       json.dumps({"id": "cmake-clang-v1", "version": "1.0.20"}))
        r = run(probe, sandbox, env, "install", zip_path)
        if "Failed to extract" in r.stdout:
            print("SKIP: install case: no zip extractor on the closed PATH\n" + r.stdout.strip())
        else:
            want = "The new toolchain pack did not pass its check: %s." % STEP_CMAKE
            check("ok=0" in r.stdout and want in r.stdout,
                  "an install of a pack whose cmake does not run names the step", r.stdout.strip())
    finally:
        remove_tree(tmp)

    if failures:
        print("%d check(s) failed" % failures)
        return 1
    print("PASS: the setup host names the failed toolchain step and keeps PATH bounded")
    return 0


if __name__ == "__main__":
    sys.exit(main())
