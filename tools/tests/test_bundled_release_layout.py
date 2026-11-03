#!/usr/bin/env python3
"""Releases ship the compiled game built from COMMITTED generated/ C.

WHY THIS EXISTS
===============
Until 2026-09-30 the release flow was "setup-host": CI wiped generated/,
shipped emitters + sources + a wizard, and every player regenerated and
rebuilt the title on their own machine. That shipped uncompiled work. The
pivot to bundled releases touches four places that can drift back
independently -- the CI template, the scaffold's .gitignore, the scaffold's
packager wrapper, and Project Studio's audit/apply -- so this test pins each
one to the bundled contract rather than trusting the docs.

Every check here is against the ARTIFACT (the template text, the wrapper it
emits, the script's exit status on a fixture tree), never the intent.

Run:  python3 tools/tests/test_bundled_release_layout.py
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

FW = Path(__file__).resolve().parents[2]
TOOLKIT = FW / "tools" / "new_project_layout"
sys.path.insert(0, str(TOOLKIT))

TEMPLATE = FW / "docs" / "ci" / "templates" / "game-release.yml"
CHECK_GENERATED = FW / "tools" / "ci" / "check_generated.sh"
BOOT = "SLUS_012.34"
GIT = shutil.which("git")
BASH = shutil.which("bash")
if os.name == "nt":
    # PATH can resolve bash to the WSL launcher and git to an MSYS shim.
    # Neither reliably accepts the native Windows fixture/script paths.
    git_root = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "Git"
    if (git_root / "cmd/git.exe").is_file():
        GIT = str(git_root / "cmd/git.exe")
    if (git_root / "bin/bash.exe").is_file():
        BASH = str(git_root / "bin/bash.exe")


def _git(root, *args):
    return subprocess.run([GIT, "-C", str(root), *args], capture_output=True,
                          text=True, check=False)


def _fixture_title(root: Path, *, ignore_generated=False, track=True,
                   retail=False):
    """A minimal game repo with committed generated C."""
    root.mkdir(parents=True, exist_ok=True)
    _git(root, "init", "-q")
    _git(root, "config", "user.email", "t@example.invalid")
    _git(root, "config", "user.name", "t")
    (root / "game.toml").write_text(
        f'[game]\nname = "Fixture"\nexe = "disc/{BOOT}"\n'
        '[runtime]\noverlay_cache = true\n', encoding="utf-8")
    (root / "CMakeLists.txt").write_text(
        'psxrecomp_add_game_runtime(psx-runtime\n'
        f'    GEN_MARKER "generated/{BOOT}_dispatch.c"\n'
        f'    GEN_FULL_GLOB "generated/{BOOT}_full_*.c"\n)\n', encoding="utf-8")
    gen = root / "generated"
    gen.mkdir(exist_ok=True)
    (gen / f"{BOOT}_dispatch.c").write_text("/* dispatch */\n", encoding="utf-8")
    (gen / f"{BOOT}_full_00.c").write_text("/* full */\n", encoding="utf-8")
    if retail:
        # A stray SCPH stem in the TITLE's generated/ is harmless to the gate
        # now (BIOS C is the framework's business, see test_fails_on_retail_bios_c).
        (gen / "SCPH1001_dispatch.c").write_text("/* retail */\n", encoding="utf-8")
        (gen / "SCPH1001_full.c").write_text("/* retail */\n", encoding="utf-8")
    (root / ".gitignore").write_text(
        ("/generated/\n" if ignore_generated else "") + "/disc/\n/bios/\n/dist/\n/analysis/\n",
        encoding="utf-8")
    _git(root, "add", "game.toml", "CMakeLists.txt", ".gitignore")
    if track and not ignore_generated:
        _git(root, "add", "generated")
    _git(root, "commit", "-q", "-m", "fixture")
    return root


class TemplateContract(unittest.TestCase):
    def setUp(self):
        self.text = TEMPLATE.read_text(encoding="utf-8")

    def test_template_exists_under_its_new_name(self):
        self.assertTrue(TEMPLATE.is_file(), TEMPLATE)
        self.assertFalse((TEMPLATE.parent / "setup-release.yml").exists(),
                         "the setup-host template must not linger beside the bundled one")

    def test_never_clears_generated(self):
        self.assertNotIn("clear_generated", self.text)
        self.assertFalse((FW / "tools" / "ci" / "clear_generated.sh").exists(),
                         "clear_generated.sh wipes the release's inputs; it must be gone")

    def test_gates_on_committed_game_c_before_building(self):
        self.assertIn("tools/ci/check_generated.sh", self.text)
        self.assertLess(self.text.index("check_generated.sh"),
                        self.text.index("Configure & build the game"))

    def test_links_committed_bios_backends(self):
        self.assertNotIn("generate_openbios", self.text, "BIOS C is committed, not emitted in CI")
        self.assertIn("-DPSXRECOMP_BIOS_STALE_FATAL=ON", self.text)
        self.assertIn("BIOS backends linked: .*SCPH1001", self.text)

    def test_configures_a_full_build(self):
        self.assertIn("-DPSXRECOMP_REQUIRE_GAME_C=ON", self.text)
        self.assertNotIn("-DPSXRECOMP_FORCE_SETUP_HOST=ON", self.text)
        self.assertNotIn("-DPSXRECOMP_ALLOW_NO_BIOS=ON", self.text)
        self.assertIn("linking generated game C (full runtime)", self.text)
        self.assertIn("BIOS backends linked: .*OpenBIOS", self.text)

    def test_packages_through_the_bundled_wrapper(self):
        self.assertIn("scripts/package_release.sh build-ci", self.text)
        self.assertNotIn("package_setup_release.sh", self.text)
        self.assertNotIn("package_setup_host.sh", self.text)

    def test_zip_verification_rejects_kit_content(self):
        for pat in ("'^psxrecomp/'", "'^psxrecomp_cli.py'", "'^generated/'", "'^psxrecomp-game'"):
            self.assertIn(pat, self.text, f"zip gate must reject {pat}")
        self.assertIn("bios/openbios.bin", self.text)

    def test_parses_as_yaml_when_pyyaml_is_available(self):
        try:
            import yaml  # noqa: F401
        except ImportError:
            self.skipTest("pyyaml not installed")
        import yaml
        doc = yaml.safe_load(self.text)
        self.assertEqual(set(doc["jobs"]), {"prepare", "build", "release"})
        names = [s.get("name", "") for s in doc["jobs"]["build"]["steps"]]
        self.assertIn("Verify committed game C", names)
        self.assertIn("Package game zip", names)


class ScaffoldContract(unittest.TestCase):
    def test_gitignore_templates_track_generated(self):
        for p in (TOOLKIT / "templates" / "gitignore.in",
                  FW / "docs" / "ci" / "templates" / "game.gitignore"):
            lines = p.read_text(encoding="utf-8").splitlines()
            ignored = [ln for ln in lines if re.match(r"^\s*/?generated/?\s*$", ln)]
            self.assertEqual(ignored, [], f"{p} ignores generated/")

    def test_packager_wrapper_template_is_bundled(self):
        tpl = TOOLKIT / "templates" / "package_release.sh.in"
        self.assertTrue(tpl.is_file(), tpl)
        self.assertFalse((TOOLKIT / "templates" / "package_setup_release.sh.in").exists())
        text = tpl.read_text(encoding="utf-8")
        self.assertIn("package_game_release.sh", text)
        self.assertNotIn("package_setup_host.sh", text)
        self.assertNotIn("--project-dir", text, "a bundled release ships no sources")

    def test_scaffold_scripts_use_the_new_template_and_wrapper(self):
        for script in (TOOLKIT / "setup_project.sh", TOOLKIT / "setup_project.ps1"):
            text = script.read_text(encoding="utf-8")
            self.assertIn("game-release.yml", text, script)
            self.assertIn("package_release.sh.in", text, script)
            self.assertNotIn("setup-release.yml", text, script)
            self.assertNotIn("package_setup_release", text, script)
            self.assertIn("git add generated", text, script)

    def test_fill_tokens_fills_the_template(self):
        with tempfile.TemporaryDirectory() as td:
            out = Path(td) / "release.yml"
            r = subprocess.run([sys.executable, str(TOOLKIT / "fill_tokens.py"),
                                str(TEMPLATE), str(out), "--ci-placeholders",
                                "--set", "ZIP_PREFIX=fx", "--set", "GAME_TITLE=Fixture: Recompiled"],
                               capture_output=True, text=True, check=False)
            self.assertEqual(r.returncode, 0, r.stderr)
            text = out.read_text(encoding="utf-8")
            self.assertNotIn("YOUR_ZIP_PREFIX", text)
            self.assertNotIn("YOUR_GAME_TITLE", text)
            self.assertIn("dist/fx-*-${{ matrix.artifact }}.zip", text)
            self.assertIn('name: "Fixture: Recompiled ${{ needs.prepare.outputs.tag }}"', text)


class CheckGeneratedScript(unittest.TestCase):
    def run_check(self, root):
        return subprocess.run([BASH, CHECK_GENERATED.as_posix(), "--root", root.as_posix()],
                              capture_output=True, text=True, check=False)

    def test_passes_on_committed_game_c(self):
        with tempfile.TemporaryDirectory() as td:
            root = _fixture_title(Path(td) / "t")
            r = self.run_check(root)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("generated C ok", r.stdout)

    def test_fails_when_marker_is_missing(self):
        with tempfile.TemporaryDirectory() as td:
            root = _fixture_title(Path(td) / "t")
            (root / "generated" / f"{BOOT}_dispatch.c").unlink()
            r = self.run_check(root)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("is missing", r.stderr)

    def test_fails_when_generated_is_ignored(self):
        with tempfile.TemporaryDirectory() as td:
            root = _fixture_title(Path(td) / "t", ignore_generated=True)
            r = self.run_check(root)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("NOT tracked", r.stderr)

    def test_fails_when_generated_is_untracked(self):
        with tempfile.TemporaryDirectory() as td:
            root = _fixture_title(Path(td) / "t", track=False)
            r = self.run_check(root)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("NOT tracked", r.stderr)

    def test_fails_on_retail_bios_c(self):
        # Retail BIOS C is committed by the FRAMEWORK and linked (decision
        # 2026-09-30); a copy in the title's generated/ is not a failure.
        with tempfile.TemporaryDirectory() as td:
            root = _fixture_title(Path(td) / "t", retail=True)
            r = self.run_check(root)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_fails_when_framework_bios_backend_is_missing(self):
        with tempfile.TemporaryDirectory() as td:
            root = _fixture_title(Path(td) / "t")
            (root / "psxrecomp" / "generated").mkdir(parents=True)
            (root / "psxrecomp" / "generated" / "OpenBIOS_full.c").write_text("x", encoding="utf-8")
            r = self.run_check(root)
            self.assertNotEqual(r.returncode, 0)
            self.assertRegex(r.stderr, r"psxrecomp/generated/OpenBIOS_(dispatch\.c is missing|full\.c exists but is not tracked)")


class ProjectStudioContract(unittest.TestCase):
    def test_audit_flags_ignored_generated_and_missing_c(self):
        from project_studio.detect import audit_project
        with tempfile.TemporaryDirectory() as td:
            root = _fixture_title(Path(td) / "t", ignore_generated=True)
            report = audit_project(root)
            by_id = {c.id: c for c in report.checks}
            self.assertEqual(by_id["gitignore"].status.value, "fail")
            self.assertEqual(by_id["generated_committed"].status.value, "fail")

    def test_audit_passes_committed_generated(self):
        from project_studio.detect import audit_project
        with tempfile.TemporaryDirectory() as td:
            root = _fixture_title(Path(td) / "t")
            report = audit_project(root)
            by_id = {c.id: c for c in report.checks}
            self.assertEqual(by_id["generated_committed"].status.value, "pass")
            self.assertNotIn("retail_bios_c", by_id)

    def test_merge_gitignore_unignores_generated(self):
        from project_studio.models import MigrateOptions
        from project_studio.ops import op_merge_gitignore
        with tempfile.TemporaryDirectory() as td:
            root = _fixture_title(Path(td) / "t", ignore_generated=True)
            res = op_merge_gitignore(root, MigrateOptions())
            self.assertTrue(res.ok, res.message)
            lines = (root / ".gitignore").read_text(encoding="utf-8").splitlines()
            self.assertFalse(any(re.match(r"^\s*/?generated/?\s*$", ln) for ln in lines))
            self.assertIn("/disc/", lines)

    def test_emit_packager_and_ci_workflow(self):
        from project_studio.models import MigrateOptions
        from project_studio.ops import op_emit_ci_workflow, op_emit_packager
        with tempfile.TemporaryDirectory() as td:
            root = _fixture_title(Path(td) / "t")
            opts = MigrateOptions(zip_prefix="fx", window_title="Fixture Recompiled",
                                  project_name="FixtureRecomp")
            pk = op_emit_packager(root, opts)
            self.assertTrue(pk.ok, pk.message)
            wrapper = root / "scripts" / "package_release.sh"
            self.assertTrue(wrapper.is_file())
            self.assertIn("package_game_release.sh", wrapper.read_text(encoding="utf-8"))
            ci = op_emit_ci_workflow(root, opts)
            self.assertTrue(ci.ok, ci.message)
            wf = (root / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8")
            self.assertIn("check_generated.sh", wf)
            self.assertIn("dist/fx-*", wf)
            # generate_ci --check agrees the installed workflow is current.
            r = subprocess.run([sys.executable, str(FW / "tools" / "generate_ci.py"),
                                str(root), "--check"], capture_output=True, text=True,
                               check=False)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            # ...and calls it stale once it carries a retired step, even though
            # it lacks nothing the template has.
            wf_path = root / ".github" / "workflows" / "release.yml"
            wf_path.write_text(wf.replace("      - name: Verify committed game C\n",
                                          "      - name: Generate OpenBIOS backend C\n"
                                          "        run: true\n"
                                          "      - name: Verify committed game C\n"),
                               encoding="utf-8")
            r = subprocess.run([sys.executable, str(FW / "tools" / "generate_ci.py"),
                                str(root), "--check"], capture_output=True, text=True,
                               check=False)
            self.assertNotEqual(r.returncode, 0, "a retired step must read as stale")
            self.assertIn("retired: Generate OpenBIOS backend C", r.stdout + r.stderr)


class PackagerScriptContract(unittest.TestCase):
    def test_bundled_packager_routes_through_release_stage(self):
        text = (FW / "tools" / "package_game_release.sh").read_text(encoding="utf-8")
        self.assertIn("release_overlay_stage.sh", text)
        self.assertIn("psx_add_mod_catalog", text)
        self.assertIn("psx_add_overlay_toolchain", text)
        self.assertIn("PSXRECOMP_FORCE_SETUP_HOST:BOOL=ON", text,
                      "must refuse to package a setup host as a game")
        # No source allowlist of any kind: nothing project-owned is shipped.
        self.assertNotIn("--project-file", text)
        self.assertNotIn("--project-dir", text)

    def test_bundled_packager_ships_recomp_ui_notices(self):
        # The executable links recomp-ui and assets/ carries its fonts and
        # images; their license and notices must ship with them.
        text = (FW / "tools" / "package_game_release.sh").read_text(encoding="utf-8")
        self.assertIn('"${STAGE}/licenses/recomp-ui-LICENSE"', text)
        self.assertIn('assets/common/${sub}/NOTICE.md', text)
        self.assertIn('"${STAGE}/assets/${sub}/NOTICE.md"', text)
        self.assertIn("RECOMP_UI_ROOT", text, "must follow the tree the build linked")
        self.assertIn('[[ -f "${UI_ROOT}/recomp_ui.cmake" ]] ||', text,
                      "missing recomp-ui tree must fail packaging")
        self.assertIn('[[ -f "${notice}" ]] ||', text,
                      "missing asset notices must fail packaging")
        template = TEMPLATE.read_text(encoding="utf-8")
        for path in ("licenses/recomp-ui-LICENSE", "assets/fonts/NOTICE.md",
                     "assets/img/NOTICE.md"):
            self.assertIn(f"'{path}'", template,
                          f"the CI zip verifier must require {path}")

    def test_scripts_are_executable_and_parse(self):
        for rel in ("tools/package_game_release.sh", "tools/ci/check_generated.sh"):
            p = FW / rel
            self.assertTrue(os.access(p, os.X_OK), f"{rel} not executable")
            r = subprocess.run([BASH, "-n", p.as_posix()], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)


class MigrateBundledHelpers(unittest.TestCase):
    """tools/migrate_bundled_release.py decides from the title's files; pin each decision."""

    def test_disc_resolution_order(self):
        from project_studio.migrate_bundled import find_disc

        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "disc").mkdir()
            self.assertIsNone(find_disc(root, None), "no disc -> None, never a guess")
            a = root / "disc" / "A.cue"
            a.write_text("", encoding="utf-8")
            self.assertEqual(find_disc(root, None), a)
            (root / "disc" / "B.cue").write_text("", encoding="utf-8")
            self.assertIsNone(find_disc(root, None), "two cues is ambiguous")
            (root / "disc.cfg").write_text("disc/B.cue\n", encoding="utf-8")
            self.assertEqual(find_disc(root, None), root / "disc" / "B.cue")
            self.assertEqual(find_disc(root, "disc/A.cue"), a, "--disc wins")
            self.assertIsNone(find_disc(root, "disc/missing.cue"))

    def test_overlay_cache_key_is_added_once_and_never_flips_false(self):
        from project_studio.migrate_bundled import ensure_overlay_cache_key

        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            gt = root / "game.toml"
            gt.write_text("[game]\nexe = \"X\"\n\n[runtime]\nbios_hle = true\n", encoding="utf-8")
            changed, _ = ensure_overlay_cache_key(root, dry_run=True)
            self.assertTrue(changed)
            self.assertNotIn("overlay_cache", gt.read_text(encoding="utf-8"), "dry-run writes nothing")
            changed, _ = ensure_overlay_cache_key(root, dry_run=False)
            self.assertTrue(changed)
            text = gt.read_text(encoding="utf-8")
            self.assertRegex(text, r"\[runtime\]\n(#[^\n]*\n)*overlay_cache = true\n")
            self.assertIn("bios_hle = true", text)
            changed, _ = ensure_overlay_cache_key(root, dry_run=False)
            self.assertFalse(changed, "idempotent")
            gt.write_text("[runtime]\noverlay_cache = false\n", encoding="utf-8")
            changed, note = ensure_overlay_cache_key(root, dry_run=False)
            self.assertFalse(changed, "an explicit false is the title's decision")
            self.assertIn("false", note)
            gt.write_text("[game]\nexe = \"X\"\n", encoding="utf-8")
            ensure_overlay_cache_key(root, dry_run=False)
            self.assertIn("[runtime]\n", gt.read_text(encoding="utf-8"), "section created when absent")

    def test_zip_prefix_is_kept_from_either_wrapper(self):
        from project_studio.migrate_bundled import zip_prefix_from_wrapper

        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "scripts").mkdir()
            self.assertEqual(zip_prefix_from_wrapper(root), "")
            (root / "scripts" / "package_setup_release.sh").write_text(
                "exec bash x.sh \\\n  --zip-prefix bpe \\\n  \"$@\"\n", encoding="utf-8")
            self.assertEqual(zip_prefix_from_wrapper(root), "bpe")
            (root / "scripts" / "package_release.sh").write_text(
                "--zip-prefix newer\n", encoding="utf-8")
            self.assertEqual(zip_prefix_from_wrapper(root), "newer", "the live wrapper wins")

    def test_refuses_without_preconditions(self):
        from project_studio.migrate_bundled import BundledMigrateOptions, migrate_to_bundled_release

        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            r = migrate_to_bundled_release(root, BundledMigrateOptions(dry_run=True))
            self.assertFalse(r.ok)
            self.assertIn("not a git repository", r.message)
            subprocess.run([GIT, "init", "-q", str(root)], check=True)
            (root / "CMakeLists.txt").write_text("project(x)\n", encoding="utf-8")
            r = migrate_to_bundled_release(root, BundledMigrateOptions(dry_run=True))
            self.assertFalse(r.ok)
            self.assertIn("psxrecomp_add_game_runtime", r.message)

    def test_wrapper_parses_and_documents_itself(self):
        p = FW / "tools" / "migrate_bundled_release.py"
        self.assertTrue(os.access(p, os.X_OK))
        r = subprocess.run([sys.executable, str(p), "--help"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        for flag in ("--psxrecomp-ref", "--skip-generate", "--push", "--dry-run", "--disc"):
            self.assertIn(flag, r.stdout)


class NativeToolchainArchiveTests(unittest.TestCase):
    def test_case_aliases_preserve_the_canonical_file(self):
        import io
        import tarfile
        sys.path.insert(0, str(FW / "tools"))
        import release_stage
        with tempfile.TemporaryDirectory(prefix="psx-python-alias-") as directory:
            root = Path(directory)
            archive = root / "python.tar"
            payload = b"synthetic terminfo data"
            with tarfile.open(archive, "w") as tar:
                entry = tarfile.TarInfo("python/share/terminfo/n/NCR260")
                entry.size = len(payload)
                entry.mode = 0o755
                tar.addfile(entry, io.BytesIO(payload))
                for name, target in (("ncr260", "NCR260"), ("other", "ncr260")):
                    entry = tarfile.TarInfo("python/share/terminfo/n/" + name)
                    entry.type = tarfile.SYMTYPE
                    entry.linkname = target
                    tar.addfile(entry)
            stale = root / "out/python/share/terminfo/n/ncr260"
            stale.parent.mkdir(parents=True)
            stale.write_bytes(b"obsolete alias")
            extracted = Path(release_stage._extract_tar_top_level(str(archive), str(root / "out"), "python"))
            for name in ("NCR260", "ncr260", "other"):
                self.assertEqual((extracted / "share/terminfo/n" / name).read_bytes(), payload)
            self.assertFalse((extracted / "share/terminfo/n/ncr260").is_symlink())
            self.assertEqual((extracted / "share/terminfo/n/ncr260").stat().st_mode,
                             (extracted / "share/terminfo/n/NCR260").stat().st_mode)

    def test_external_symlink_is_still_refused(self):
        import tarfile
        sys.path.insert(0, str(FW / "tools"))
        import release_stage
        with tempfile.TemporaryDirectory(prefix="psx-python-escape-") as directory:
            root = Path(directory)
            archive = root / "python.tar"
            with tarfile.open(archive, "w") as tar:
                entry = tarfile.TarInfo("python/bad")
                entry.type = tarfile.SYMTYPE
                entry.linkname = "../../outside"
                tar.addfile(entry)
            with self.assertRaises(tarfile.FilterError):
                release_stage._extract_tar_top_level(str(archive), str(root / "out"), "python")


if __name__ == "__main__":
    if GIT is None or BASH is None:
        print("git and bash are required", file=sys.stderr)
        sys.exit(2)
    unittest.main(verbosity=1)
