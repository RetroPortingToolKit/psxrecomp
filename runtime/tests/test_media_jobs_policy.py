"""Configure the actual media selector and check one TU and profile identity."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    p=argparse.ArgumentParser()
    p.add_argument('--cmake',required=True)
    p.add_argument('--cc',required=True)
    p.add_argument('--ninja',required=True)
    args=p.parse_args()
    root=Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix='psx-media-policy-') as temp:
        source=Path(temp)/'src';source.mkdir()
        (source/'main.c').write_text('int main(void) { return 0; }\n')
        (source/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(media_policy C)
set(PSXRECOMP_ROOT "{root.as_posix()}")
include("${{PSXRECOMP_ROOT}}/cmake/psx_execution_profile.cmake")
include("${{PSXRECOMP_ROOT}}/cmake/psx_media_jobs.cmake")
add_executable(fixture main.c "${{PSXRECOMP_ROOT}}/runtime/src/dma.c")
psxrecomp_execution_profile(fixture)
psxrecomp_media_jobs(fixture)
file(GENERATE OUTPUT "${{CMAKE_BINARY_DIR}}/selected.txt" CONTENT "$<JOIN:$<TARGET_PROPERTY:fixture,SOURCES>,\\n>")
''')

        def configure(name,profile,option=None):
            build=Path(temp)/name
            argv=[args.cmake,'-S',str(source),'-B',str(build),'-G','Ninja',
                '-DCMAKE_C_COMPILER='+args.cc,'-DCMAKE_MAKE_PROGRAM='+args.ninja,
                '-DPSX_EXECUTION_PROFILE='+profile]
            if option is not None:argv+=['-DPSX_NATIVE_MEDIA_JOBS='+option]
            result=subprocess.run(argv,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
            assert result.returncode==0,result.stdout
            sources=(build/'selected.txt').read_text().splitlines()
            dma=[x for x in sources if Path(x).name in ('dma.c','dma_media_jobs.c')]
            assert len(dma)==1,sources
            manifest=json.loads((build/'psx_execution/fixture/manifest.json').read_text())
            family=manifest['implementations']
            assert len(family)==1 and family[0]['name']=='mdec-media-jobs'
            assert family[0]['contract']=='bounded-fifo-publication-v1'
            return Path(dma[0]).name,manifest

        default,d=configure('default','ENHANCED')
        native,n=configure('on','ENHANCED','ON')
        original,o=configure('off','ENHANCED','OFF')
        reference,q=configure('reference-on','REFERENCE','ON')
        reference_off,z=configure('reference-off','REFERENCE','OFF')
        assert default==native=='dma_media_jobs.c'
        assert original==reference==reference_off=='dma.c'
        assert d==n and n['identity']!=o['identity']
        assert q==z and q['identity']!=n['identity']
        assert q['implementations']==o['implementations']
    print('PASS: default/ON native, OFF/REFERENCE original, one TU and distinct execution identities')


if __name__=='__main__':
    main()
