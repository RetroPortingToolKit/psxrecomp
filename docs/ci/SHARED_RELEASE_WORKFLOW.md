# Shared title release workflow

Call `.github/workflows/title-release.yml` at an exact engine commit. The
workflow checks out its release provider separately from the game's existing
engine submodule, so release-tool updates do not change the runtime pin.

The caller supplies product title, ZIP prefix, the existing platform matrix,
`package-kind` (`prebuilt` or `setup`), and its package script. An optional
second package script supports products with an additional lean package.
Keep genuine title tests in `extra-check-script`; common dependency setup,
emitter caches, configure/build, source checks, upload and publication stay
in the engine workflow. `provider-ref` must match the workflow's commit.

The caller keeps tag/dispatch triggers, concurrency and publication permissions.
The shared workflow preserves Windows/MSYS2, Linux and both macOS architectures.
`portable-linux` retains the pinned Ubuntu 20.04 build container and its SDK
bootstrap. The caller's original platform matrix remains authoritative.

Regional products can use `package_title_release.py package-existing` with
`[aot] mode = "audited-cache"`. Supply the exact Release build, original-disc
audit attempt and emitter. The builder checks the game ID, byte guards,
configuration tag and every staged native module digest, then runs the normal
payload, native dependency, manifest and extracted-artifact checks. Regional
disc gates and extra payload assertions remain title-owned post-stage hooks.
