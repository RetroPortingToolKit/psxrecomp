"""Build-selected host ownership; normal/REFERENCE/oracle profile selection."""
from pathlib import Path
import argparse,json,subprocess,tempfile

p=argparse.ArgumentParser()
p.add_argument('--cmake',required=True)
p.add_argument('--cc',required=True)
p.add_argument('--ninja',required=True)
a=p.parse_args()
root=Path(__file__).resolve().parents[2]
identities={}
for name,profile,option,expected in [('default','ENHANCED',None,1),('on','ENHANCED','ON',1),
        ('off','ENHANCED','OFF',0),('reference','REFERENCE','ON',0),('reference-off','REFERENCE','OFF',0)]:
    with tempfile.TemporaryDirectory(prefix='psx-resident-policy-') as scratch:
        s=Path(scratch)
        (s/'main.c').write_text('#include <stdio.h>\nint main(void){printf("%u\\n", PSX_NATIVE_RESIDENT_PREPARATION);return 0;}\n')
        (s/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(resident_policy C)
set(PSXRECOMP_ROOT "{root.as_posix()}")
include("${{PSXRECOMP_ROOT}}/cmake/psx_execution_profile.cmake")
include("${{PSXRECOMP_ROOT}}/cmake/psx_resident_preparation.cmake")
add_executable(subject main.c)
psxrecomp_execution_profile(subject)
psxrecomp_resident_preparation(subject)
get_target_property(selected subject PSX_NATIVE_RESIDENT_PREPARATION_SELECTED)
if(NOT selected EQUAL {expected})
 message(FATAL_ERROR "wrong resident preparation ownership selection")
endif()
''')
        cmd=[a.cmake,'-S',str(s),'-B',str(s/'build'),'-G','Ninja',
            '-DCMAKE_C_COMPILER='+a.cc,'-DCMAKE_MAKE_PROGRAM='+a.ninja,
            '-DPSX_EXECUTION_PROFILE='+profile]
        if option is not None:cmd+=['-DPSX_NATIVE_RESIDENT_PREPARATION='+option]
        subprocess.run(cmd,check=True,capture_output=True,text=True)
        subprocess.run([a.cmake,'--build',str(s/'build'),'--parallel','2'],check=True,capture_output=True,text=True)
        exe=s/'build/subject.exe'
        if not exe.exists():exe=s/'build/subject'
        assert int(subprocess.check_output([str(exe)],text=True))==expected
        manifest=json.loads((s/'build/psx_execution/subject/manifest.json').read_text())
        identities[name]=manifest['identity']
        print('PASS resident selection '+name,flush=True)
assert identities['default']==identities['on']==identities['off']
assert identities['reference']==identities['reference-off']
