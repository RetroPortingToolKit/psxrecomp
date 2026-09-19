#!/usr/bin/env python3
"""Compile the production retry label and following declaration as strict C11."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
parser = argparse.ArgumentParser()
parser.add_argument('--compiler', default='cc')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
text = (root/'src/overlay_loader.c').read_text(encoding='utf-8')
start = text.index('retry_candidates:')
end = text.index('int loaded_range_ci', start)
fragment = text[start:end]
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp)/'label.c'
    source.write_text('static int idx_head(int p) { return p; }\nint main(void) { int phys=0; if (phys) goto retry_candidates;\n'+fragment+'return head; }\n')
    subprocess.run([shutil.which(args.compiler) or args.compiler,'-std=c11','-pedantic-errors','-fsyntax-only',str(source)],check=True)
print('production retry label: strict C11 passes')
