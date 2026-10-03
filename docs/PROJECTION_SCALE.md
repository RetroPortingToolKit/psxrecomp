# Optional projection-distance scaling

`[video] fov_scale` accepts finite values greater than zero and at most 8;
1.0 is faithful. A valid `PSX_GTE_FOV_SCALE` overrides the config value;
invalid environment values are ignored. The whole string must be numeric
apart from surrounding whitespace. Scaling is quantized to 0.001, with a
minimum quantized denominator of 1. Values above one reduce effective H.
This scales projection distance, not the field-of-view angle. Guest H storage
is unchanged; RTPS/RTPT and their optional precision shadows use effective H.

The setting is loaded by a runtime-only parser extension. It does not change
the shared recompiler parser, emitted BIOS/game C or emitter fingerprints.
Consuming it requires updating the framework pin and rebuilding the runtime;
neither game nor BIOS regeneration is required. Bundled OpenBIOS is supported.
