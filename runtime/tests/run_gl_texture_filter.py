"""Compile and check palette-aware minification on a hidden real GL context."""
import argparse
import json
import os
from pathlib import Path
import platform
import subprocess
import sys

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc',required=True)
    parser.add_argument('--sdl-include',required=True)
    parser.add_argument('--sdl-library',required=True)
    parser.add_argument('--output',required=True)
    parser.add_argument('--fixture',type=Path)
    parser.add_argument('--hd-pack', action='store_true')
    parser.add_argument('--cxx')
    parser.add_argument('--argument', action='append', default=[])
    parser.add_argument('--build-only',action='store_true',
                        help='build the probe without starting a GL context')
    parser.add_argument('--optimization',choices=['0','1','2','3'],default='1')
    args=parser.parse_args()
    fw=Path(__file__).resolve().parents[2]
    out=Path(args.output).resolve(); out.mkdir(parents=True,exist_ok=True)
    receipt=[]
    def run(command,environment=None):
        command=[str(x) for x in command]
        env=os.environ.copy()
        if environment: env.update(environment)
        r=subprocess.run(command,cwd=out,capture_output=True,text=True,encoding='utf-8',errors='replace',env=env)
        receipt.append(dict(cmd=command,env=environment,exit=r.returncode,stdout=r.stdout,stderr=r.stderr))
        (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8')
        print(r.stdout,r.stderr[-4000:],flush=True)
        if r.returncode: raise subprocess.CalledProcessError(r.returncode,command)
    flags=['-std=gnu11','-O'+args.optimization,'-DPSX_SDL3=1','-DPSX_NO_DEBUG_TOOLS=1',
           '-ffunction-sections','-fdata-sections','-I',fw/'runtime/include','-I',fw/'runtime/src']
    for include in args.sdl_include.split(';'):
        if include: flags+=['-I',include]
    sources=[('probe',args.fixture.resolve() if args.fixture else fw/'runtime/tests/test_gl_texture_filter.c'),
             ('sw',fw/'runtime/src/gpu_sw_renderer.c'),('fi',fw/'runtime/src/frame_interpolation.c'),
             ('rp',fw/'runtime/src/render_pass_plan.c')]
    if (fw/'runtime/src/psx_openxr.c').exists():
        sources.append(('xr',fw/'runtime/src/psx_openxr.c'))
    objects=[]
    for name,source in sources:
        obj=out/(name+'.o'); run([args.cc,*flags,'-c',source,'-o',obj]); objects.append(obj)
    if args.hd_pack:
        if not args.cxx: parser.error('--hd-pack requires --cxx')
        cppflags=[arg if arg!='-std=gnu11' else '-std=c++17' for arg in flags]
        for name in ('gpu_hd_textures','hd_texture_pack','duckstation_texture_pack'):
            obj=out/(name+'.o')
            run([args.cxx,*cppflags,'-c',fw/'runtime/src'/(name+'.cpp'),'-o',obj])
            objects.append(obj)
    exe=out/('filter.exe' if platform.system()=='Windows' else 'filter')
    libraries=['-lm']
    if platform.system()=='Windows':
        libraries+=['-static','-lopengl32']+['-l'+x for x in ('kernel32','user32','gdi32','winmm','imm32',
            'ole32','oleaut32','version','uuid','advapi32','setupapi','shell32','dinput8')]
    elif platform.system()=='Darwin':
        from run_gl_scale_invariance import MAC_FRAMEWORKS
        for framework in MAC_FRAMEWORKS: libraries+=['-framework',framework]
        libraries+=['-liconv','-Wl,-dead_strip']
    else: libraries+=['-lGL','-ldl','-lpthread']
    if platform.system()!='Darwin': libraries+=['-Wl,--gc-sections']
    run([args.cxx if args.hd_pack else args.cc,*objects,args.sdl_library,'-o',exe,*libraries])
    if not args.build_only:
        arguments=list(args.argument)
        if args.hd_pack:
            pack=out/'pack'; (pack/'replacements').mkdir(parents=True,exist_ok=True)
            (pack/'beetle'/'demo-texture-replacements').mkdir(parents=True,exist_ok=True)
            arguments=[pack]
        run([exe,*arguments],{'PSX_GL_HIRES_WINDOW':'0'} if args.hd_pack else None)
        if args.hd_pack: run([exe,*arguments],{'PSX_GL_HIRES_WINDOW':'1'})
        if args.hd_pack: run([exe,'--native-baseline'],{'PSX_GL_HIRES_WINDOW':'1'})
    return 0

if __name__=='__main__':
    try: sys.exit(main())
    except subprocess.CalledProcessError as exc: sys.exit(exc.returncode)
