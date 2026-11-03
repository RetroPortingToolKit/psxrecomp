"""Windows integration gate using real hidden runtime processes.

Provide a disposable working directory/config and explicit BIOS/disc. Neither
the title's release tree nor real memory cards are used. Logs are retained in
--output; child processes are always stopped by their owned process handles.
"""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import time

p=argparse.ArgumentParser()
for name in ('exe','bios','disc','workdir','output'):
    p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args()
for name in ('exe','bios','disc','workdir','output'):
    setattr(a,name,getattr(a,name).resolve())
if os.name != 'nt':
    raise SystemExit('This integration gate targets Winsock on Windows.')
a.output.mkdir(parents=True,exist_ok=True)
children=[]
streams=[]
result={}
def launch(name,port):
    out=a.output/(name+'.stdout.log'); err=a.output/(name+'.stderr.log')
    stdout=out.open('wb'); stderr=err.open('wb'); streams.extend((stdout,stderr))
    cards=a.output/(name+'-cards'); cards.mkdir(exist_ok=True)
    env=dict(os.environ,SDL_AUDIODRIVER='dummy',PSX_OVERLAY_AUTOCOMPILE_OFF='1')
    child=subprocess.Popen([str(a.exe),'--headless','--no-launcher','--bios',str(a.bios),
                            '--disc',str(a.disc),'--memcard-dir',str(cards),
                            '--debug-port',str(port)],cwd=a.workdir,env=env,
                           creationflags=subprocess.CREATE_NO_WINDOW,stdout=stdout,stderr=stderr)
    children.append(child)
    return child,out
def wait_log(child,path,needle):
    end=time.monotonic()+20
    while time.monotonic()<end:
        text=path.read_text(errors='replace')
        if needle in text: return text
        if child.poll() is not None: raise AssertionError((child.returncode,text))
        time.sleep(.1)
    raise TimeoutError((needle,path.read_text(errors='replace')))
def stop(child):
    if child.poll() is None:
        child.terminate(); child.wait(timeout=10)
def unused_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1',0)); return s.getsockname()[1]
def owners(port):
    code=f'@(Get-NetTCPConnection -LocalPort {port} -State Listen -ErrorAction SilentlyContinue | Select-Object -ExpandProperty OwningProcess) | ConvertTo-Json -Compress'
    output=subprocess.check_output(['powershell.exe','-NoProfile','-Command',code],text=True).strip()
    value=json.loads(output) if output else []
    return value if isinstance(value,list) else [value]

try:
    # An existing legacy reusable listener must not be hijacked by this runtime.
    with socket.socket() as legacy:
        legacy.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
        legacy.bind(('127.0.0.1',0)); legacy.listen()
        port=legacy.getsockname()[1]
        child,log=launch('occupied-legacy',port)
        wait_log(child,log,f'debug server bind({port}) FAILED')
        assert owners(port)==[os.getpid()],owners(port)
        result['legacy_listener_preserved']=True
        stop(child)

    port=unused_port()
    first,log=launch('first',port)
    wait_log(first,log,f'LISTENING on 127.0.0.1:{port}')
    assert owners(port)==[first.pid],owners(port)
    with socket.socket() as competitor:
        competitor.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
        try:
            competitor.bind(('127.0.0.1',port))
        except OSError as error:
            assert error.winerror in (10013,10048),error
            result['reusable_competitor_error']=error.winerror
        else:
            raise AssertionError('A competing SO_REUSEADDR bind unexpectedly succeeded')
    second,log=launch('second',port)
    wait_log(second,log,f'debug server bind({port}) FAILED')
    assert owners(port)==[first.pid],owners(port)
    result['second_runtime_refused']=True
    stop(second); stop(first)
    restarted,log=launch('restarted',port)
    wait_log(restarted,log,f'LISTENING on 127.0.0.1:{port}')
    assert owners(port)==[restarted.pid],owners(port)
    result['restart_after_owned_shutdown']=True
finally:
    for child in children: stop(child)
    for stream in streams: stream.close()
    result['owned_processes_stopped']=all(c.poll() is not None for c in children)
    (a.output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result))
