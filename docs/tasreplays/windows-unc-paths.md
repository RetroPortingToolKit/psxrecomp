# Windows network asset paths

Pepsiman's first native attempt exited before guest execution because BIOS
selection could not open the path returned by its resolver. An independent
fixture using the same WinLibs GCC 16.1 UCRT library reproduced the boundary:
the original `\\server\share\file` was readable, `path.is_absolute()` returned
false, and `std::filesystem::absolute()` returned `D:\server\share\file` without
an error. Python's argument list had preserved the original argument.

The handover corpus already records a similar network-path symptom under
PSX-WIN-003. This reproduction isolates a filesystem resolution defect in
addition to that finding's separate PowerShell argument-boundary problem.
Microsoft documents UNC paths as fully qualified in
[Windows file path formats](https://learn.microsoft.com/en-us/dotnet/standard/io/file-path-formats).

`host_path.h` preserves fully qualified Windows network and device paths
before invoking the standard library's relative-path resolution. Runtime
BIOS, disc and config resolution, CUE payload resolution, and compiler config
and BIOS CLI paths use that shared rule. Relative config values still resolve
against their config root; ordinary relative CLI paths still resolve against
the current directory. No emulated device or timing behavior changes.

`host_path_test` is registered with the recompiler CTest suite. It checks both
UNC separator forms, device namespace paths, mapped drives, relative paths,
config-root isolation and error-code clearing. Its optional path argument
also checks that an owned asset remains readable, without changing it.
The campaign additionally compiled the production disc resolver and verified
that both network spellings preserve the Pepsiman CUE and all eight tracks.
The existing disc resolver, CD-DA and SBI fixtures pass with this correction.

This is a host admission correction, not a TAS pass or PS1 hardware claim.
Full native observations and registered title regressions remain required.
