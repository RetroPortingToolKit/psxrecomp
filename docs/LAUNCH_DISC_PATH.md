# Launch disc selection

The launch adapter uses the existing disc-path resolver before mounting player media.
A usable CUE remains selected and retains its track metadata; a raw image can resolve to its owning CUE.
Unaccompanied raw images, CHD, and the existing unusable-CUE fallback retain their resolver behavior.

The source-owned `runtime/tests/test_launch_disc_path.py` extracts the exact production adapter and compiles it with the production resolver and CUE parser.
Eight synthetic cases cover 12-track mixed media, eight-track audio-only media, CUE and BIN entry points, raw-only, CHD passthrough, broken CUE fallback, and missing files.
The original adapter loses the track map in four cases; the correction passes all eight and checks each track number, audio/data type, and start position.
This is an adapter/parser regression; it does not establish full controller behavior or game playback.

Local corpus: PSX-CD-012, with Vib-Ribbon and Wipeout XL as two consumers.
Exact-title web searches found audio conversion tools and emulator issues but no applicable earlier PSXRecomp launch-adapter repair.
Use the existing resolver rather than introducing another CUE parser.
