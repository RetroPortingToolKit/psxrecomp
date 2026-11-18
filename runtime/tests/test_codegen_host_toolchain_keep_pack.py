#!/usr/bin/env python3
"""Regression: the setup host keeps a toolchain pack that may be in use.

host/psxrecomp_codegen_host.c used to remove a `latest` folder whose pack
failed one check, rename the active pack to <tag>.broken, and install a
downloaded pack by deleting the installed tag first. A build running from
that pack then kept only the files it held open. It also had no way to run
without touching the toolchain cache at all.

This test builds a probe that includes the host source and calls its toolchain
functions on a made-up cache in a closed environment: every data, cache and
temporary folder the host reads points inside the test's temporary folder,
and PATH holds the system folders only. The pack's cmake is a program that
exits 1, so every check of it fails. The cases:

  * a failed check of `latest` (heal) leaves the pack and the pointer as
    they were;
  * a failed check of the active pack (discard) leaves it as it was;
  * an install whose new pack fails its check leaves the installed pack as it
    was;
  * with PSXRECOMP_TOOLCHAIN_READONLY=1 a prune, a move of the latest pointer
    and a write or removal of the project's stamp change nothing (each is
    first run without the switch to show that it does change something).
"""

from pathlib import Path
import hashlib
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
EXE = ".exe" if WINDOWS else ""
CMAKE = "cmake" + EXE
SWITCH = "PSXRECOMP_TOOLCHAIN_READONLY"

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
    if (strcmp(op, "heal") == 0) {
        heal_broken_toolchain_pointers();
        printf("note=%s\n", g_tc_repair_note);
    } else if (strcmp(op, "discard") == 0 && argc > 2) {
        snprintf(g_toolchain_bin, sizeof(g_toolchain_bin), "%s", argv[2]);
        discard_unhealthy_active_toolchain();
        printf("note=%s\n", g_tc_repair_note);
    } else if (strcmp(op, "install") == 0 && argc > 2) {
        char err[600] = "";
        int ok = host_install_toolchain_from_zip(argv[2], NULL, NULL, err, sizeof(err));
        printf("ok=%d\nerr=%s\n", ok, err);
    } else if (strcmp(op, "prune") == 0 && argc > 2) {
        prune_old_toolchain_tags(argv[2]);
    } else if (strcmp(op, "latest") == 0 && argc > 3) {
        printf("ok=%d\n", set_toolchain_latest_pointer(argv[2], argv[3]));
    } else if (strcmp(op, "stamp-clear") == 0) {
        clear_project_toolchain_stamp();
    } else if (strcmp(op, "stamp-write") == 0 && argc > 2) {
        printf("ok=%d\n", write_project_toolchain_stamp(argv[2]));
    } else {
        fprintf(stderr, "probe: unknown operation\n");
        return 2;
    }
    return 0;
}
"""

FAILING_CMAKE_C = "int main(void) { return 1; }\n"

SANDBOX_ROOTS = {
    "LOCALAPPDATA": "localappdata",
    "APPDATA": "appdata",
    "USERPROFILE": "home",
    "HOME": "home",
    "XDG_DATA_HOME": "xdg-data",
    "RETCOMM_DATA_HOME": "data-home",
    # The first cache base whose path names "retcomm" is where the host
    # installs and prunes; this one is listed first.
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


def is_link(p):
    if os.path.islink(p):
        return True
    isjunction = getattr(os.path, "isjunction", None)
    if isjunction and isjunction(p):
        return True
    try:
        attrs = getattr(os.lstat(p), "st_file_attributes", 0)
    except OSError:
        return False
    return bool(attrs & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400))


def snapshot(root):
    """Every entry under root: folders, links (with their target), file bytes."""
    out = {}
    if not root.exists():
        return out
    for dirpath, dirnames, filenames in os.walk(root):
        base = Path(dirpath)
        for d in list(dirnames):
            p = base / d
            rel = p.relative_to(root).as_posix()
            if is_link(p):
                try:
                    out[rel] = "link -> " + os.readlink(p)
                except OSError:
                    out[rel] = "link"
                dirnames.remove(d)
            else:
                out[rel] = "dir"
        for f in filenames:
            p = base / f
            rel = p.relative_to(root).as_posix()
            if is_link(p):
                out[rel] = "link"
            else:
                out[rel] = hashlib.sha256(p.read_bytes()).hexdigest()
    return out


def describe_change(before, after):
    lines = []
    for k in sorted(set(before) | set(after)):
        if before.get(k) != after.get(k):
            lines.append("    %s: %s -> %s" % (k, before.get(k, "(absent)"), after.get(k, "(absent)")))
    return "\n".join(lines[:12])


def make_pack(folder, version, cmake_bytes, marker):
    (folder / "bin").mkdir(parents=True, exist_ok=True)
    cmake = folder / "bin" / CMAKE
    cmake.write_bytes(cmake_bytes)
    cmake.chmod(0o755)
    (folder / "retcomm-toolchain.json").write_text(
        json.dumps({"id": "cmake-clang-v1", "version": version}), encoding="utf-8")
    (folder / marker).write_text(marker, encoding="utf-8")


def remove_tree(path):
    """Remove a sandbox, links as links (never what a junction points at)."""
    if not path.exists() and not is_link(path):
        return
    for dirpath, dirnames, filenames in os.walk(path, topdown=True):
        for d in list(dirnames):
            p = Path(dirpath) / d
            if is_link(p):
                if WINDOWS:
                    os.rmdir(p)
                else:
                    os.unlink(p)
                dirnames.remove(d)
    shutil.rmtree(path, ignore_errors=True)


class Case:
    def __init__(self, tmp, name, probe, cmake_bytes):
        self.sandbox = tmp / name
        self.env = closed_environment(self.sandbox)
        self.cache = self.sandbox / SANDBOX_ROOTS["RETCOMM_TOOLCHAIN_CACHE"]
        self.project = self.sandbox / "project"
        self.probe = probe
        self.cmake_bytes = cmake_bytes

    def run(self, *args, readonly=False):
        env = dict(self.env)
        if readonly:
            env[SWITCH] = "1"
        return subprocess.run([str(self.probe)] + [str(a) for a in args], capture_output=True,
                              text=True, encoding="utf-8", errors="replace", env=env,
                              cwd=str(self.sandbox), timeout=120)

    def state(self):
        return {"cache": snapshot(self.cache), "project": snapshot(self.project)}


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
    if WINDOWS:
        fake_c = tmp / "fake_cmake.c"
        fake_c.write_text(FAILING_CMAKE_C, encoding="utf-8")
        fake = tmp / "fake_cmake.exe"
        res = subprocess.run([cc, "-o", str(fake), str(fake_c)], capture_output=True,
                             text=True, encoding="utf-8", errors="replace")
        if res.returncode != 0:
            print("FAIL: could not build the failing cmake\n" + res.stderr[-2000:])
            return None, None
        return probe, fake.read_bytes()
    return probe, b"#!/bin/sh\nexit 1\n"


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

    tmp = Path(tempfile.mkdtemp(prefix="tc-keep-pack-"))
    try:
        probe, cmake_bytes = build(cc, ui, tmp)
        if probe is None:
            return 1

        # 1. A failed check of `latest` (a real folder that holds a pack).
        c = Case(tmp, "heal", probe, cmake_bytes)
        make_pack(c.cache / "latest", "1.0.20", cmake_bytes, "pack-in-use.txt")
        before = c.state()["cache"]
        r = c.run("heal")
        after = c.state()["cache"]
        check(after == before, "a failed check of latest leaves the pack and the pointer as they were",
              describe_change(before, after) + "\n" + r.stdout + r.stderr)
        check("not removed" in r.stdout,
              "the note after a failed check says the pack was not removed", r.stdout.strip())

        # 2. A failed check of the active pack (a versioned tag).
        c = Case(tmp, "discard", probe, cmake_bytes)
        make_pack(c.cache / "1.0.20", "1.0.20", cmake_bytes, "pack-in-use.txt")
        before = c.state()["cache"]
        r = c.run("discard", c.cache / "1.0.20" / "bin")
        after = c.state()["cache"]
        check(after == before, "a failed check of the active pack leaves it where it is",
              describe_change(before, after) + "\n" + r.stdout + r.stderr)

        # 3. An install whose new pack fails its check, over the same tag.
        c = Case(tmp, "install", probe, cmake_bytes)
        make_pack(c.cache / "1.0.20", "1.0.20", cmake_bytes, "pack-in-use.txt")
        zip_path = c.sandbox / "new-pack.zip"
        with zipfile.ZipFile(zip_path, "w") as z:
            info = zipfile.ZipInfo("bin/" + CMAKE)
            info.external_attr = (0o755 | stat.S_IFREG) << 16
            z.writestr(info, cmake_bytes)
            z.writestr("retcomm-toolchain.json",
                       json.dumps({"id": "cmake-clang-v1", "version": "1.0.20"}))
        before = c.state()["cache"]
        r = c.run("install", zip_path)
        after = c.state()["cache"]
        if "Failed to extract" in r.stdout:
            print("SKIP: install case: no zip extractor on the closed PATH\n" + r.stdout.strip())
        else:
            check("ok=0" in r.stdout, "an install of a pack that fails its check reports a failure",
                  r.stdout.strip())
            check(after == before, "an install of a pack that fails its check leaves the installed pack",
                  describe_change(before, after) + "\n" + r.stdout + r.stderr)

        # 4. PSXRECOMP_TOOLCHAIN_READONLY=1 changes nothing.
        def readonly_case(name, prepare, args):
            for readonly in (False, True):
                c = Case(tmp, "%s-%s" % (name, "readonly" if readonly else "plain"), probe, cmake_bytes)
                prepare(c)
                before = c.state()
                r = c.run(*args(c), readonly=readonly)
                after = c.state()
                if readonly:
                    check(after == before, "%s changes nothing under %s=1" % (name, SWITCH),
                          describe_change(before["cache"], after["cache"]) +
                          describe_change(before["project"], after["project"]) + "\n" + r.stderr)
                    check("not done" in r.stderr, "%s says on stderr what it left undone" % name,
                          r.stderr.strip()[-400:])
                else:
                    check(after != before, "%s changes something without the switch" % name,
                          r.stdout + r.stderr)

        def two_tags(c):
            make_pack(c.cache / "1.0.20", "1.0.20", cmake_bytes, "old.txt")
            make_pack(c.cache / "1.0.21", "1.0.21", cmake_bytes, "new.txt")

        def stamp(c):
            (c.project / "toolchain").mkdir(parents=True, exist_ok=True)
            (c.project / "toolchain" / ".psxrecomp-bin").write_text("x\n", encoding="utf-8")

        readonly_case("a prune of older tags", two_tags, lambda c: ["prune", c.cache / "1.0.21"])
        readonly_case("a move of the latest pointer", two_tags,
                      lambda c: ["latest", c.cache, c.cache / "1.0.21"])
        readonly_case("a removal of the project's stamp", stamp, lambda c: ["stamp-clear"])
        readonly_case("a write of the project's stamp", lambda c: None,
                      lambda c: ["stamp-write", c.cache / "1.0.21" / "bin"])
    finally:
        remove_tree(tmp)

    if failures:
        print("%d check(s) failed" % failures)
        return 1
    print("PASS: the setup host keeps a pack that may be in use and honours %s" % SWITCH)
    return 0


if __name__ == "__main__":
    sys.exit(main())
