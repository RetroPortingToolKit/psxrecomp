from pathlib import Path
p=Path(__file__).resolve().parents[2]/"runtime/src/main.cpp"
s=p.read_text(encoding="utf-8")
assert '#include "fntrace.h"' in s
assert 'extern int fntrace_is_game_started' not in s
print("fntrace C header ownership: PASS")
