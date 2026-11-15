"""Production history/RPC and the maintained device-boundary checks."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--cc',default='gcc')
    args=parser.parse_args()
    runtime=Path(__file__).resolve().parents[1]
    source=(runtime/'src/debug_server.c').read_text(encoding='utf-8')
    rpc=source[source.index('static void handle_hr_counters('):source.index('static void handle_static_call_stats(')]
    with tempfile.TemporaryDirectory(prefix='psx-headroom-') as tmp:
        work=Path(tmp)
        (work/'headroom_rpc.inc').write_text(rpc,encoding='utf-8')
        (work/'psx_observation_config.h').write_text('#define PSX_OBSERVATION_WAIT_SITE(pc) ((pc)==0x8002E6C0u)\n')
        for native in (0,1):
            for disabled in (0,1):
                for opt in ('-O0','-O2'):
                    exe=work/('counters.exe' if os.name=='nt' else 'counters')
                    defs=[f'-DPSX_DISABLE_HEADROOM_COUNTERS={disabled}','-DPSX_NO_DEBUG_TOOLS']
                    if native: defs+=['-DPSX_OBSERVATION_TIME=1']
                    common=[args.cc,'-std=c11',opt,'-w',*defs,'-I',str(work),'-I',str(runtime/'include')]
                    subprocess.run(common+[str(runtime/'tests/test_headroom_counters.c'),
                        str(runtime/'src/headroom_counters.c'),'-o',str(exe)],check=True)
                    result=subprocess.run([str(exe)],capture_output=True,text=True,check=True,timeout=30)
                    replies=[json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
                    assert len(replies)==2
                    first,last=replies
                    assert first['enabled']==(not disabled) and len(first['history'])==(0 if disabled else 128)
                    assert last['history']==[] and first['missing'] and first['canonical_memory_supported']
                    assert len(first['fields'])==len(set(first['fields'])) and not first['memory_supported']
                    for row in first['history']:
                        assert len(row['counts'])==len(first['fields'])
                        assert row['counts'][first['fields'].index('dispatch')]==3
                        assert row['counts'][first['fields'].index('mem_helper_load_mmio_u16')]==2
                    subprocess.run(common+[str(runtime/'tests/test_psx_cycle_event_boundaries.c'),
                        str(runtime/'src/psx_cycles.c'),'-o',str(exe)],check=True)
                    subprocess.run([str(exe)],check=True,timeout=30)
                    print(f'PASS native={native} disabled={disabled} {opt}: history/RPC and production clock')


if __name__=='__main__': main()
