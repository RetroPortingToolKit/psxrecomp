"""Actual original/native whole preparations, API observations and full pack bytes."""
import argparse
from pathlib import Path
import subprocess
import tempfile

p=argparse.ArgumentParser()
p.add_argument('--reference',required=True)
p.add_argument('--native',required=True)
a=p.parse_args()
cases=['stock','modded_allow','modded_reject','callback_fail','wrong_file',
       'unreadable','size_limit','invalid_spec','publish_error']
with tempfile.TemporaryDirectory(prefix='psx-resident-whole-job-') as scratch:
    root=Path(scratch)
    for case in cases:
        sides=[]
        for name,exe in [('reference',a.reference),('native',a.native)]:
            out=root/(name+'-'+case)
            subprocess.run([exe,str(out),case],check=True)
            sides.append(out)
        assert (sides[0]/'observations.bin').read_bytes()==(sides[1]/'observations.bin').read_bytes(),case
        packs=[{f.name:f.read_bytes() for f in side.glob('cache/ResidentOwnedTest/*.pack')}
               for side in sides]
        assert packs[0]==packs[1],case+' complete serialized pack differs'
        assert bool(packs[0])==(case in ('stock','modded_allow')),case
        print('PASS original/native whole-job observations and full pack: '+case,flush=True)
