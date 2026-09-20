# External PS1 conformance fixtures

This harness runs exact, pinned public PS-X EXE tests through PSXRecomp and
compares the guest console output with the fixture's expected PS1 log. It is a
small regression oracle, not a replacement for matched hardware capture.

Initial fixtures from `JaCzekanski/ps1-tests` release `build-158`:

- `cpu-cop`: 17 CPU exception/coprocessor-access assertions.
- `gte-test-all`: 1,150 GTE register/opcode vectors.

The MIT fixture binaries are not vendored. `fixtures.json` records their exact
release, paths, and SHA-256 identities. The synthetic boot disc additionally
requires `license_data.dat` extracted locally from a PlayStation disc owned by
the operator; that file and every prepared run remain outside Git.

## Prepare

Build `psxrecomp-toml`, `psxrecomp-game`, and an OpenBIOS backend first. Then:

```powershell
python .\tools\conformance_testrom\conformance.py prepare `
  --fixture cpu-cop `
  --source-root D:\private\ps1-tests-build-158 `
  --output D:\validation\cpu-cop-v1 `
  --framework-root D:\src\psxrecomp `
  --recompiler-build D:\build\psxrecomp-recompiler `
  --mkpsxiso D:\tools\mkpsxiso.exe `
  --license-data D:\private\license_data.dat
```

The command refuses an occupied output directory, verifies every private input,
generates a synthetic licensed disc, recompiles the test, and writes a
hash-bound `prepare-receipt.json`.

## Build and run

```powershell
cmake -S .\tools\conformance_testrom -B D:\build\cpu-cop `
  -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=C:\mingw64\bin\gcc.exe `
  -DCMAKE_CXX_COMPILER=C:\mingw64\bin\g++.exe `
  -DCONFORMANCE_RUN_DIR=D:\validation\cpu-cop-v1 `
  -DCONFORMANCE_DEBUG_PORT=4601
cmake --build D:\build\cpu-cop --target psx-conformance

python .\tools\conformance_testrom\conformance.py run `
  --run-dir D:\validation\cpu-cop-v1 `
  --executable D:\build\cpu-cop\PSXConformance.exe `
  --bios .\bios\openbios.bin `
  --port 4601
```

Use the same MinGW toolchain as the runtime. A default Windows Clang/MSVC
configuration may select MASM for libchdr's optional assembly and fail when
`ml64.exe` is not installed.

`run` starts the runtime headlessly, retrieves the bounded guest-TTY ring over
the existing JSON debug protocol, performs an exact normalized comparison, and
writes `result.json`. Normalization removes capture-format `% ` prefixes, blank
outer lines, CR/LF differences, and the derived `Total tests:` line which some
binaries print but their pinned expected logs omit. Assertion text and pass/fail
counts remain exact; the complete normalized capture is retained in the result.

## Oracle status

The release's expected `psx.log` is the first automated oracle. Promotion to a
hardware-qualified result requires Eagle to run the same hashed `test.bin` and
capture the same normalized console result or another unambiguous pass surface.
