from pathlib import Path
import argparse,subprocess,tempfile
p=argparse.ArgumentParser();p.add_argument('--cmake',default='cmake');p.add_argument('--cc',required=True);p.add_argument('--generator',default='Ninja');p.add_argument('--make-program');a=p.parse_args()
root=Path(__file__).resolve().parents[2]
for name,profile,option,expected in [('default','ENHANCED',None,1),('on','ENHANCED','ON',1),('off','ENHANCED','OFF',0),('reference','REFERENCE','ON',0),('reference-off','REFERENCE','OFF',0)]:
 with tempfile.TemporaryDirectory(prefix='psx-host-pacing-') as scratch:
  s=Path(scratch);(s/'dummy.c').write_text('int dummy(void){return 0;}\n')
  (s/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(host_pacing_policy C)
set(PSXRECOMP_ROOT "{root.as_posix()}")
include("${{PSXRECOMP_ROOT}}/cmake/psx_host_pacing.cmake")
add_library(subject STATIC dummy.c)
set_property(TARGET subject PROPERTY PSX_EXECUTION_PROFILE {profile})
psxrecomp_host_pacing(subject)
get_target_property(selected subject PSX_NATIVE_HOST_PACING)
if(NOT selected EQUAL {expected})
 message(FATAL_ERROR "wrong host pacing selection")
endif()
get_target_property(sources subject SOURCES)
list(FILTER sources INCLUDE REGEX "host_deadline[.]c$")
list(LENGTH sources bodies)
if(NOT bodies EQUAL {expected})
 message(FATAL_ERROR "wrong host wait body count")
endif()
''')
  cmd=[a.cmake,'-S',str(s),'-B',str(s/'build'),'-G',a.generator,'-DCMAKE_C_COMPILER='+a.cc]
  if a.make_program:cmd+=['-DCMAKE_MAKE_PROGRAM='+a.make_program]
  if option is not None:cmd+=['-DPSX_NATIVE_HOST_PACING='+option]
  subprocess.run(cmd,check=True,capture_output=True,text=True)
 print(name+' passed')
