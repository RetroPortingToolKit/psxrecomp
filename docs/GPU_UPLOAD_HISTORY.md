# GPU upload diagnostic retention

GPU A0 upload diagnostics retain the first 128 uploads. Once the history is
full, subsequent uploads continue normally without changing retained entries.
Reset the active capture slot before every upload so a full history cannot
keep incrementing the last retained entry's signed word counter.

The authored regression compiles the production capture code at O0 and O2,
checks retention at capacity, and exercises the largest legal upload. It
requires a GCC-compatible compiler and no BIOS or game assets:

```sh
python runtime/tests/test_gpu_upload_history.py --cc gcc
```

The defect caused a long Biohazard TAS replay to crash after return 214410.
The campaign fix passed that boundary and executed all 239202 returns. That
run still first differed from its reference at return 233568 (one clock early,
nine terminal RAM bytes different). This diagnostic fix is not a claim of
Biohazard replay fidelity. It requires rebuilding the runtime, not regenerating
game code; game framework pins are not changed by this submission.
