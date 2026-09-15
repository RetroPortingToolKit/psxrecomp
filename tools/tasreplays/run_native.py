"""Run isolated PSXRTI1/PSXRTI2 playback and retain process and input evidence.

The native observer owns input checkpoints. A clean process exit alone does
not qualify playback. Default execution has no window and uses software video.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import time
from input_contexts import decode as decode_contexts, check_protected_effects


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_json(path, value):
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2)
        stream.write("\n")


def route_identity(path):
    if path.stat().st_size > 24 + 12 * 1000000:
        raise ValueError('route exceeds frame capacity')
    data = path.read_bytes()
    if len(data) < 24:
        raise ValueError("short route")
    magic, version, size, count, reserved = struct.unpack_from("<8sIIII", data)
    if magic == b'PSXRTI2\0':
        if (version, size, reserved) != (2, 12, 0) or not 0 < count <= 1000000 or len(data) != 24+12*count:
            raise ValueError('invalid DualShock route header/count/size')
        original, protocol = hashlib.sha256(), hashlib.sha256()
        previous, steps = None, 0
        for index in range(count):
            sequence, buttons, ly, lx, ry, rx, analog, zero = struct.unpack_from('<IH6B', data, 24+12*index)
            if sequence != index+1 or zero or analog:
                raise ValueError('DualShock sequence/reserved bytes or unqualified physical Analog press')
            row = struct.pack('<H5B', buttons, ly, lx, ry, rx, analog)
            steps += row != previous; previous = row
            if steps > 4096:
                raise ValueError('DualShock step capacity exceeded')
            original.update(row)
            # Nyma expands axes to u16 with value<<8; the device rescales to u8.
            axes = [((value << 8)*255+32767)//65535 for value in (ly,lx,ry,rx)]
            protocol.update(struct.pack('<H5B', buttons, *axes, analog))
        return {'format':'PSXRTI2', 'frames':count, 'steps':steps,
                'sha256':hashlib.sha256(data).hexdigest(),
                'original_controller_sha256':original.hexdigest(),
                'expected_protocol_sha256':protocol.hexdigest()}
    if (magic, version, size, reserved) != (b"PSXRTI1\0", 1, 8, 0):
        raise ValueError("unsupported route header")
    if not 0 < count <= 1000000 or len(data) != 24 + 8 * count:
        raise ValueError("route frame count or size")
    words = bytearray()
    for index in range(count):
        sequence, word, zero = struct.unpack_from("<IHH", data, 24 + 8 * index)
        if sequence != index + 1 or zero:
            raise ValueError("route sequence or reserved bytes")
        words.extend(struct.pack("<H", word))
    return {"frames": count, "sha256": digest(path),
            "words_sha256": hashlib.sha256(words).hexdigest()}


def card_identity(path):
    if path.stat().st_size != 131072:
        raise ValueError('card1 must be an exact raw 128 KiB card')
    return {'path':str(path), 'bytes':131072, 'sha256':digest(path)}


def validate_card_model(model):
    if model != 'nymashock-1.29.0':
        raise ValueError('card replay requires the qualified explicit card model')


def playback_identity_matches(complete, identity, tail):
    if (not isinstance(complete, dict) or complete.get('frame') != identity['frames']+tail or
            complete.get('input_frames') != identity['frames'] or complete.get('neutral_tail_ticks') != tail):
        return False
    if identity.get('format') == 'PSXRTI2':
        return (complete.get('controller_profile') == 'nymashock-2.9.1-dualshock-neutral-analog' and
                complete.get('original_controller_sha256') == identity['original_controller_sha256'] and
                complete.get('applied_controller_sha256') == complete.get('expected_protocol_sha256') == identity['expected_protocol_sha256'])
    return complete.get('applied_words_sha256') == identity['words_sha256']


def source_clock_identity(path, toc_model, seek_model, read_model):
    if path is None:
        return None
    if (toc_model, seek_model, read_model) != (
            'octoshock-2.2.2', 'octoshock-2.2.2', 'octoshock-2.2.2-pipeline'):
        raise ValueError('source clock tape requires source TOC, explicit seek and pipeline models')
    path = path.resolve(strict=True)
    data = path.read_bytes()
    if len(data) < 20 or data[:16] != b'PSX-CD-RNG1\0\0\0\0\0':
        raise ValueError('invalid source clock tape header')
    count = struct.unpack_from('<I', data, 16)[0]
    if not 1 <= count <= 1048576 or len(data) != 20 + count * 4:
        raise ValueError('invalid source clock tape count or length')
    return {'path': str(path), 'sha256': hashlib.sha256(data).hexdigest(), 'words': count}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_directory", type=Path)
    for name in ("exe", "game", "route", "disc", "bios"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument('--card1', type=Path, help='raw initial card1; staged as a fresh writable copy and verified in the loaded peripheral')
    parser.add_argument('--card-model', choices=('default','nymashock-1.29.0'), default='default')
    parser.add_argument("--timeout", type=float, default=900)
    parser.add_argument("--show", action="store_true")
    parser.add_argument("--speed", choices=("1", "2", "4", "8", "16", "32", "64", "max"), default="1",
                        help="visible replay fast-forward cap; headless runs are always uncapped")
    parser.add_argument("--renderer", choices=("software", "opengl"), default="software",
                        help="rendering backend; window visibility is independent")
    parser.add_argument("--fast-boot", action="store_true")
    parser.add_argument("--hle", action="store_true")
    parser.add_argument("--scheduler", choices=("hle", "lle"), default="hle")
    parser.add_argument("--neutral-tail", type=int, default=0,
                        help="verified neutral SIO samples after unchanged route EOF")
    parser.add_argument("--debug-port", type=int, default=4394)
    parser.add_argument("--sio-trace", action="store_true")
    parser.add_argument("--update-profile", type=Path,
                        help="explicit experimental accepted-update clock configuration and route receipt")
    parser.add_argument("--update-decisions",type=Path,
                        help="measured poll acceptance decisions from a bounded clean-boot compilation")
    parser.add_argument("--update-predictor",choices=("gate", "previous-accept"),default="gate",
                        help="experimental prediction only; actual packet/context acceptance stays authoritative")
    parser.add_argument("--checkpoint-every", type=int, default=300)
    parser.add_argument('--storage-budget-mib',type=int,help='Stop with host_storage_budget if diagnostic output exceeds this bound')
    parser.add_argument("--watch-u16", type=lambda x: int(x, 0), action="append", default=[],
                        help="read a physical RAM u16 before each next input (max32)")
    parser.add_argument("--record-frame", type=int,
                        help="arm existing ordered access recorder for one runtime frame; export watched accesses")
    parser.add_argument("--cpu-state", action="store_true",
                        help="read-only CPU and IRQ state at each before-next-input boundary")
    parser.add_argument("--video-state", action="store_true",
                        help="passive display mode, field and video-clock state at each before-next-input boundary")
    parser.add_argument("--instruction-histogram",type=int,nargs=2,metavar=("FIRST","LAST"),
                        help="passive instruction-fetch counts for one to eight runtime input-boundary intervals")
    parser.add_argument("--instruction-site",type=lambda x:int(x,0),action="append",default=[],
                        help="ordered passive timestamps for up to 16 selected fetch PCs, capped at 8192 per interval")
    parser.add_argument('--cpu-boundary-window',type=int,nargs=2,metavar=('LOW','HIGH'),
                        help='source-profile passive pre-fetch CPU scalars in a bounded absolute cycle interval')
    parser.add_argument('--gpu-command-window',type=int,nargs=2,metavar=('LOW','HIGH'),
                        help='Bounded read-only GPU command diagnostic')
    parser.add_argument('--cpu-return-probe',action='store_true',
                        help='passive CPU scalars at original-model frontend return boundaries')
    parser.add_argument('--ram-page-probe',action='store_true',
                        help='passive hashes of all512 RAM pages at original-model frontend returns')
    parser.add_argument('--ram-snapshot-frame',type=int,action='append',default=[],
                        help='also save raw RAM at a selected frontend return (requires page probe; max64)')
    parser.add_argument("--read-watch", type=lambda x: int(x, 0), nargs=2,
                        metavar=("LOW", "HIGH"), help="record existing RAM reads in physical [LOW,HIGH)")
    parser.add_argument("--cd-read-start-model", choices=("default", "octoshock-2.2.2-pipeline"), default="default",
                        help="explicit source-core pipeline comparison; not full timing compatibility")
    parser.add_argument('--cd-drive-model',choices=('default','nymashock-1.29.0'),default='default',
                        help='experimental Nymashock drive head, seek, Pause and Reset timing; cold diagnostics only')
    parser.add_argument('--cd-source-clock-tape', type=Path,
                        help='experimental command/seek clock using an immutable raw random-word tape')
    parser.add_argument('--cd-cdda-model',choices=('default','octoshock-2.3'),default='default',
                        help='source CDDA single-speed seek, two-sector pipe and report ownership')
    parser.add_argument('--mdec-source-model',choices=('default','octoshock-2.3','nymashock-1.29.0'),default='default',
                        help='source MDEC FIFO/work and request DMA model; color output only')
    parser.add_argument('--cd-dma-model',choices=('default','octoshock-2.2.2'),default='default',
                        help='experimental manual CD DMA service and CPU wait; cold boot only')
    parser.add_argument("--cd-toc-seek-model", choices=("default", "octoshock-2.2.2"), default="default",
                        help="ReadTOC deterministic source seek lower bound; jitter and exact head position omitted")
    parser.add_argument("--cd-explicit-seek-model", choices=("default", "octoshock-2.2.2"), default="default",
                        help="SeekL/P source lower bound with paused/standby state; physical head and jitter remain approximate")
    parser.add_argument("--cd-firmware-model", choices=("default", "octoshock-2.2.2"), default="default",
                        help="source PU-18 Test20 identity; does not change seek timing")
    parser.add_argument("--cd-cold-status-model", choices=("default", "octoshock-2.2.2"), default="default",
                        help="source cold sticky shell-open bit cleared after first GetStat; cold boot only")
    parser.add_argument("--syscall-model", choices=("default", "guest-exception"), default="default",
                        help="route every syscall through the installed guest exception handler")
    parser.add_argument("--critical-section-model", choices=("direct", "exception"), default="direct",
                        help="experimental non-nested SYS01/02 guest exception execution")
    parser.add_argument("--field-model", choices=("default", "octoshock-2.2.2-ntsc-fields", "octoshock-2.2.2-ntsc-raster"), default="default",
                        help="experimental cold-boot source field durations; no state restore or PAL")
    parser.add_argument('--gpu-status-model', choices=('default','octoshock-2.2.2-raster'), default='default',
                        help='source GPUSTAT field/line bits only; requires the NTSC raster clock')
    parser.add_argument("--pad-ack-model", choices=("default", "octoshock-2.2.2-digital", "nymashock-1.29.0-dualshock"), default="default",
                        help="Experimental source digital-pad ACK delay/pulse; cold boot only")
    parser.add_argument("--dma-model", choices=("default", "octoshock-2.2.2-otc"), default="default",
                        help="experimental source OTC service and CPU-wait rule; cold boot only")
    parser.add_argument('--gpu-dma-model', choices=('default','octoshock-2.2.2-vram-upload','octoshock-2.2.2-bounded-linked-list','octoshock-2.2.2-bounded-quad'), default='default',
                        help='source GPU DMA timing; bounded-quad adds the cold automatic FIFO/work projection with strict draw-state guards')
    parser.add_argument("--timer1-model",choices=("default","octoshock-2.2.2"),default="default",
                        help="experimental source timer1 clock/sync; IRQ-enabled modes and restore reject")
    parser.add_argument("--timer2-model",choices=("default","octoshock-2.2.2"),default="default",
                        help="experimental source timer2 clock/IRQ state; requires source timer1; timer0 IRQ modes and restore reject")
    parser.add_argument("--precise-slice", choices=("off", "on"), default="off",
                        help="Experimental existing instruction-boundary IRQ slicer; RAM BIOS guard requires the qualified candidate emitter")
    args = parser.parse_args()
    if args.speed != "1" and not args.show:
        parser.error('--speed requires --show; headless playback is already uncapped')
    if args.ram_page_probe and args.field_model!='octoshock-2.2.2-ntsc-raster':
        raise ValueError('RAM page probe requires source raster frontend boundaries')
    if args.ram_snapshot_frame and (not args.ram_page_probe or len(args.ram_snapshot_frame)>64 or
            len(set(args.ram_snapshot_frame))!=len(args.ram_snapshot_frame) or
            any(f<1 for f in args.ram_snapshot_frame)):
        raise ValueError('RAM snapshots require page probe and 1..64 unique positive frames')
    if args.timer2_model!="default" and args.timer1_model!="octoshock-2.2.2":
        raise ValueError("timer2 model requires source timer1 model")
    if args.timer1_model!="default" and args.field_model!="octoshock-2.2.2-ntsc-raster":
        raise ValueError("timer1 model requires NTSC raster field model")
    if args.gpu_status_model!='default' and args.field_model!='octoshock-2.2.2-ntsc-raster':
        raise ValueError('GPU status model requires NTSC raster field model')
    if args.syscall_model!='default' and args.gpu_dma_model!='octoshock-2.2.2-bounded-quad':
        raise ValueError('guest syscall model requires the source CPU/service profile')
    if args.cd_drive_model!='default':
        if not args.cd_source_clock_tape:
            raise ValueError('source drive model requires a clock tape and cold diagnostics')
    if args.cd_cdda_model!='default' and not args.cd_source_clock_tape:
        raise ValueError('source CDDA requires an explicit source clock tape')
    if args.mdec_source_model!='default' and args.gpu_dma_model!='octoshock-2.2.2-bounded-quad':
        raise ValueError('source MDEC requires the qualified source service scheduler')
    if args.gpu_dma_model=='octoshock-2.2.2-bounded-quad' and args.field_model!='octoshock-2.2.2-ntsc-raster':
        raise ValueError('GPU quad model requires NTSC raster field model')
    if (args.update_decisions or args.update_predictor != "gate") and not args.update_profile:
        raise ValueError('update decisions require an explicit update profile')
    if not 0 <= args.neutral_tail <= 60000:
        raise ValueError("neutral tail outside 0..60000 ticks")
    if not 1 <= args.checkpoint_every <= 10000:
        raise ValueError("checkpoint interval outside 1..10000")
    if args.storage_budget_mib is not None and not 1<=args.storage_budget_mib<=16384:
        raise ValueError('storage budget must be in 1..16384 MiB')
    if len(args.watch_u16) > 32 or len(set(args.watch_u16)) != len(args.watch_u16) or any(
            a < 0 or a > 0x1ffffe or a % 2 for a in args.watch_u16):
        raise ValueError("watch addresses must be unique aligned physical RAM u16 offsets (max32)")
    if args.record_frame is not None and (args.record_frame < 0 or not args.watch_u16):
        raise ValueError("record-frame requires nonnegative frame and configured watches")
    if args.instruction_histogram:
        first,last=args.instruction_histogram
        if not 0<=first<=last<=60000 or last-first>7:
            raise ValueError('histogram requires one to eight bounded runtime intervals')
    if args.instruction_site and (not args.instruction_histogram or len(args.instruction_site)>16 or len(set(args.instruction_site))!=len(args.instruction_site) or any(p<0 or p>0xfffffffc or p%4 for p in args.instruction_site)):
        raise ValueError('instruction sites require histogram and unique aligned 32-bit PCs (max16)')
    if args.read_watch and (args.record_frame is None or
                           not 0 <= args.read_watch[0] < args.read_watch[1] <= 0x200000 or
                           args.read_watch[1] - args.read_watch[0] > 4096):
        raise ValueError("read-watch requires record-frame and a RAM range of at most 4096 bytes")
    paths = {name: getattr(args, name).resolve(strict=True)
             for name in ("exe", "game", "route", "disc", "bios")}
    clock_tape = source_clock_identity(args.cd_source_clock_tape, args.cd_toc_seek_model,
                                       args.cd_explicit_seek_model, args.cd_read_start_model)
    if clock_tape:
        paths['cd_source_clock_tape'] = Path(clock_tape['path'])
    identity = route_identity(paths["route"])
    dualshock = identity.get('format') == 'PSXRTI2'
    if dualshock and (args.update_profile or args.pad_ack_model == 'octoshock-2.2.2-digital'):
        raise ValueError('DualShock does not admit retiming or the digital Octoshock ACK model')
    initial_card = None
    if args.card1:
        validate_card_model(args.card_model)
        paths['card1'] = args.card1.resolve(strict=True)
        initial_card = card_identity(paths['card1'])
    elif args.card_model != 'default':
        raise ValueError('card model requires an explicit initial card1')
    # The completion hook exits before the final return observer. Bind every
    # enabled RAM capture to the exact declared number of observed returns.
    ram_returns = identity['frames'] + args.neutral_tail - 1
    if (args.ram_page_probe or args.cpu_return_probe) and not 1 <= ram_returns <= 1000000:
        raise ValueError('RAM capture requires 1..1000000 declared completed returns')
    if any(frame > ram_returns for frame in args.ram_snapshot_frame):
        raise ValueError('RAM snapshot exceeds the declared completed-return boundary')
    update_profile = None
    update_contexts = None
    context_values = None
    protected_mask = None
    if args.update_profile:
        update_profile=json.loads(args.update_profile.read_text())
        if update_profile['schema']!='psx-input-update-profile-v1':
            raise ValueError('unsupported update profile')
        receipt=Path(update_profile['derivation_receipt'])
        derived=json.loads(receipt.read_text())
        if digest(receipt)!=update_profile['derivation_sha256'] or derived['route']!=identity:
            raise ValueError('update route/derivation identity mismatch')
        if derived.get('contexts'):
            update_contexts=derived['contexts']
            context_path=Path(update_contexts['path'])
            if digest(context_path)!=update_contexts['sha256']:
                raise ValueError('source context sidecar changed')
            context_values=decode_contexts(context_path.read_bytes(),identity['frames'])
        if 'protected_raw_mask' in update_profile:
            protected_mask=update_profile['protected_raw_mask']
            if context_values is None or not isinstance(protected_mask,int) or not 0<protected_mask<=65535:
                raise ValueError('protected raw mask requires source contexts and a nonzero u16 mask')
    refresh_guard = None
    if update_profile and 'neutral_refresh_guard' in update_profile:
        refresh_guard=update_profile['neutral_refresh_guard']
        if protected_mask is None or not isinstance(refresh_guard,list) or len(refresh_guard)!=4 or any(
            not isinstance(v,int) or v<0 or (v>0xffffffff or v%4 if i==1 else v>0x1ffffe or v%2)
            for i,v in enumerate(refresh_guard)):
            raise ValueError('invalid neutral refresh guard')
    run = args.run_directory.resolve()
    run.mkdir(exist_ok=False, parents=True)
    # This runtime loads settings and card paths beside argv[0], not CWD.
    # A private copy also freezes the executable while later builds proceed.
    launch_exe = run / paths["exe"].name
    shutil.copyfile(paths["exe"], launch_exe)
    if digest(launch_exe) != digest(paths["exe"]):
        raise ValueError("staged executable differs")
    staged_card = None
    if initial_card:
        (run / 'cards').mkdir()
        staged_card = run / 'cards/card1.mcd'
        shutil.copyfile(paths['card1'], staged_card)
        if digest(staged_card) != initial_card['sha256']:
            raise ValueError('staged initial card differs')
    route_path = paths['route']
    if dualshock:
        route_path = run / 'input.psxrti2'
        shutil.copyfile(paths['route'], route_path)
        if digest(route_path) != identity['sha256']:
            raise ValueError('staged controller route differs')
    renderer = args.renderer
    settings = f'''[video]
renderer = "{renderer}"
supersampling = 1
window_width = 960
antialiasing = false
texture_filtering = "nearest"
crt_filter = "raw"
auto_skip_fmv = false
turbo_loads = false
fast_boot = {str(args.fast_boot).lower()}
bios_hle = {str(args.hle).lower()}
fullscreen = 0
frame_interpolation = false
aspect_ratio = "4:3"
adaptive_view = false
[audio]
spu_hq = false
[launcher]
skip_launcher = true
[bios]
path = "{paths['bios'].as_posix()}"
[disc]
path = "{paths['disc'].as_posix()}"
[memcard]
dir = "cards"
card1 = "cards/card1.mcd"
card2 = "cards/card2.mcd"
enable1 = {str(initial_card is not None).lower()}
enable2 = false
[controller]
p1_device = "keyboard"
p1_mode = "digital"
p2_device = "none"
p2_mode = "digital"
'''
    (run / "settings.toml").write_text(settings, encoding="utf-8")
    command = [str(launch_exe), "--game", str(paths["game"]),
               "--disc", str(paths["disc"]), "--bios", str(paths["bios"]),
               "--no-launcher", "--debug-port", str(args.debug_port),
               "--renderer", renderer]
    if not args.show:
        command.append("--headless")
    # A replay cannot inherit unrelated diagnostic, load-state or enhancement
    # switches from another task. Retain the complete selected PSX environment.
    env = {k: v for k, v in os.environ.items() if not k.startswith("PSX_")}
    selected_env = {"PSX_INPUT_ROUTE_FILE": str(route_path),
                    "PSX_INPUT_ROUTE_CAPTURE_DIR": str(run),
                    "PSX_HLE_SCHEDULER": "1" if args.scheduler == "hle" else "0",
                    "PSX_LOW_LATENCY_INPUT": "0",
                    "PSX_INPUT_ROUTE_NEUTRAL_TAIL": str(args.neutral_tail),
                    "PSX_INPUT_ROUTE_TRACE": "1" if args.sio_trace else "0",
                    "PSX_INPUT_ROUTE_CAPTURE_EVERY": str(args.checkpoint_every),
                    "PSX_BIOS_HLE": "1" if args.hle else "0"}
    if args.speed != "1":
        selected_env["PSX_FAST_FORWARD"] = "1"
        selected_env["PSX_FAST_FORWARD_SPEED"] = args.speed
    if refresh_guard is not None:
        selected_env["PSX_INPUT_UPDATE_REFRESH_GUARD"]=','.join(map(str,refresh_guard))
    if args.cd_read_start_model != "default":
        selected_env["PSX_CD_READ_START_MODEL"] = args.cd_read_start_model
    if clock_tape:
        selected_env['PSX_CD_SOURCE_CLOCK_TAPE'] = clock_tape['path']
    if args.cd_drive_model!='default':
        selected_env['PSX_CD_DRIVE_MODEL']=args.cd_drive_model
    if args.cd_cdda_model!='default':
        selected_env['PSX_CD_CDDA_MODEL']=args.cd_cdda_model
    if args.mdec_source_model!='default':
        selected_env['PSX_MDEC_SOURCE_MODEL']=args.mdec_source_model
    if args.cd_dma_model!='default':
        selected_env['PSX_CD_DMA_MODEL']=args.cd_dma_model
    if args.cd_toc_seek_model != "default":
        selected_env["PSX_CD_TOC_SEEK_MODEL"] = args.cd_toc_seek_model
    if args.cd_explicit_seek_model != "default":
        selected_env["PSX_CD_EXPLICIT_SEEK_MODEL"] = args.cd_explicit_seek_model
    if args.cd_firmware_model != "default":
        selected_env["PSX_CD_FIRMWARE_MODEL"] = args.cd_firmware_model
    if args.cd_cold_status_model != "default":
        selected_env["PSX_CD_COLD_STATUS_MODEL"] = args.cd_cold_status_model
    if args.syscall_model != "default":
        selected_env["PSX_SYSCALL_MODEL"] = args.syscall_model
    if args.critical_section_model != "direct":
        selected_env["PSX_CRITICAL_SECTION_MODEL"] = args.critical_section_model
    if args.field_model != "default":
        selected_env["PSX_INPUT_ROUTE_FIELD_MODEL"] = args.field_model
    if args.gpu_status_model!='default':
        selected_env['PSX_GPU_STATUS_MODEL']=args.gpu_status_model
    if args.timer1_model!='default':
        selected_env['PSX_TIMER1_MODEL']=args.timer1_model
    if args.timer2_model!='default':
        selected_env['PSX_TIMER2_MODEL']=args.timer2_model
    selected_env['PSX_PRECISE_SLICE']='1' if args.precise_slice=='on' else '0'
    if args.gpu_dma_model!='default':
        selected_env['PSX_GPU_DMA_MODEL']=args.gpu_dma_model
    if args.pad_ack_model != "default":
        selected_env["PSX_INPUT_ROUTE_PAD_ACK_MODEL"] = args.pad_ack_model
    if initial_card:
        selected_env['PSX_INPUT_ROUTE_CARD_MODEL'] = args.card_model
        selected_env['PSX_INPUT_ROUTE_CARD1_SHA256'] = initial_card['sha256']
    if args.dma_model != "default":
        selected_env["PSX_INPUT_ROUTE_DMA_MODEL"] = args.dma_model
    if args.instruction_histogram:
        selected_env['PSX_INPUT_HISTOGRAM_RANGE']=','.join(map(str,args.instruction_histogram))
    if args.instruction_site:
        selected_env['PSX_INPUT_HISTOGRAM_SITES']=','.join(f'{p:08X}' for p in args.instruction_site)
    if args.cpu_boundary_window:
        low,high=args.cpu_boundary_window
        if low<0 or high<=low or high-low>1000000:raise ValueError('invalid CPU boundary window')
        selected_env['PSX_SOURCE_CPU_BOUNDARY_WINDOW']=f'{low},{high}'
    if args.gpu_command_window:
        low,high=args.gpu_command_window
        if low<0 or high<=low or high-low>1000000:raise ValueError('invalid GPU command window')
        selected_env['PSX_SOURCE_GPU_COMMAND_WINDOW']=f'{low},{high}'
    if args.cpu_return_probe:
        selected_env['PSX_SOURCE_CPU_RETURN_PROBE']='1'
        selected_env['PSX_SOURCE_CPU_MAX_FRAMES']=str(ram_returns)
    if args.ram_page_probe:
        selected_env['PSX_SOURCE_RAM_PAGE_PROBE']='1'
        selected_env['PSX_SOURCE_RAM_MAX_FRAMES']=str(ram_returns)
    if args.ram_snapshot_frame:
        selected_env['PSX_SOURCE_RAM_SNAPSHOT_FRAMES']=','.join(map(str,args.ram_snapshot_frame))
    if args.update_predictor != "gate":
        selected_env["PSX_INPUT_UPDATE_PREDICTOR"] = args.update_predictor
    if args.watch_u16:
        selected_env["PSX_INPUT_ROUTE_WATCH_U16"] = ",".join(hex(a) for a in args.watch_u16)
    if args.record_frame is not None:
        selected_env["PSX_RECORD_FRAME"] = str(args.record_frame)
    if args.cpu_state:
        selected_env["PSX_INPUT_ROUTE_CPU_STATE"] = "1"
    if args.video_state:
        selected_env["PSX_INPUT_ROUTE_VIDEO_STATE"] = "1"
    if args.read_watch:
        selected_env["PSX_READ_WATCH"] = ",".join(hex(a) for a in args.read_watch)
    if update_profile:
        fields=('marker_address','marker_pc','marker_ra','gate_address','packet_address',
                'header_address','expected_header','max_vblank_ticks','stall_vblank_ticks',
                'marker_register','marker_register_value','auxiliary_register_value')
        selected_env['PSX_INPUT_UPDATE_CLOCK']=','.join(str(update_profile[k]) for k in fields)
    if args.update_decisions:
        shutil.copyfile(args.update_decisions,run/'update-decisions.txt')
        selected_env['PSX_INPUT_UPDATE_DECISIONS']=str(run/'update-decisions.txt')
    if update_contexts:
        shutil.copyfile(update_contexts['path'],run/'input.contexts')
        selected_env['PSX_INPUT_UPDATE_CONTEXTS']=str(run/'input.contexts')
    if protected_mask is not None:
        selected_env['PSX_INPUT_UPDATE_PROTECTED_RAW_MASK']=str(protected_mask)
    env.update(selected_env)
    write_json(run / "manifest.json", {
        "schema": "psx-native-tas-run-v1", "command": command,
        "requested_speed": args.speed if args.show else "headless-uncapped",
        "update_profile":update_profile,
        "update_contexts":update_contexts,
        "update_profile_sha256":digest(args.update_profile) if update_profile else None,
        "update_decisions_sha256":digest(args.update_decisions) if args.update_decisions else None,
        "inputs": {name: {"path": str(path), "sha256": digest(path)}
                   for name, path in paths.items()},
        "route": identity, "psx_environment": selected_env,
        "initial_card1":initial_card,
        "staged_card1":str(staged_card) if staged_card else None,
        "settings_sha256": digest(run / "settings.toml"),
        "launcher_sha256": digest(Path(__file__)),
        "boundary": "N records supplied before the next normal VBlank sample",
        "guest_controller_polls_measured": False,
        "source_timing_equivalence": "unqualified", "timeout_seconds": args.timeout,
        "storage_budget_mib":args.storage_budget_mib})
    startup = None
    if os.name == "nt":
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 1 if args.show else 0
    start = time.monotonic()
    timed_out = False
    with (run / "stdout.log").open("xb") as out, (run / "stderr.log").open("xb") as err:
        process = subprocess.Popen(command, cwd=run, env=env, startupinfo=startup,
                                   stdout=out, stderr=err)
        write_json(run / "process.json", {"pid": process.pid})
        print(f"Native PID {process.pid}; evidence: {run}", flush=True)
        from process_budget import wait_budgeted
        budget=wait_budgeted(process,run,args.timeout,
                             args.storage_budget_mib*1024**2 if args.storage_budget_mib is not None else None)
        code=budget['exit_code']
        timed_out=budget['stop_reason']=='host_timeout'
    write_json(run / "exit.json", {"pid": process.pid, "exit_code": code,
               "stop_reason":budget['stop_reason'],"diagnostic_inventory":budget['last_inventory'],
               "timed_out": timed_out, "host_seconds": time.monotonic() - start,
               "completion_exists": (run / "complete.json").exists()})
    complete = None
    if (run / "complete.json").exists():
        complete = json.loads((run / "complete.json").read_text())
    qualified = (budget['stop_reason'] is None and code == 0 and
                 playback_identity_matches(complete, identity, args.neutral_tail))
    if initial_card or dualshock:
        initial_path = run / 'initial-cards.json'
        actual_cards = json.loads(initial_path.read_text()) if initial_path.exists() else None
        expected_cards = {'card1_present':bool(initial_card), 'card2_present':False,
                          'card1_sha256':initial_card['sha256'] if initial_card else None,
                          'bytes':131072 if initial_card else 0}
        qualified = qualified and actual_cards == expected_cards
    if update_profile:
        endpath=run/'update-input-end.json'
        end=json.loads(endpath.read_text()) if endpath.exists() else None
        qualified=(budget['stop_reason'] is None and code==0 and complete is not None and end is not None
                   and end['accepted_samples']==identity['frames']
                   and end['accepted_words_sha256']==identity['words_sha256']
                   and complete['frame']==end['frame']+args.neutral_tail
                   and complete['neutral_tail_ticks']==args.neutral_tail)
        if context_values is not None:
            trace=run/'update-clock.jsonl'
            events=[json.loads(x) for x in trace.read_text().splitlines()] if trace.exists() else []
            actual=[(x['sequence'],x['context']) for x in events if x['kind']=='accepted_context']
            context_match=actual==list(enumerate(context_values,1))
            sequence_match=[i for i,_ in actual]==list(range(1,len(context_values)+1))
            data=paths['route'].read_bytes()
            words=[struct.unpack_from('<H',data,24+8*i+4)[0] for i in range(identity['frames'])]
            effects_match=(context_match if protected_mask is None else sequence_match and
                           check_protected_effects(context_values,[c for _,c in actual],words,protected_mask))
            write_json(run/'context-check.json',{'match':effects_match,'strict_context_match':context_match,
                       'protected_raw_mask':protected_mask,'actual_events':len(actual),
                       'context_differences':sum(c!=expected for (_,c),expected in zip(actual,context_values)),
                       'normal_events':sum(c==0 for _,c in actual),'auxiliary_events':sum(c==1 for _,c in actual),
                       'expected_sha256':update_contexts['sha256'],
                       'additional_neutral_refreshes':sum(x['kind']=='neutral_refresh' for x in events)})
            qualified=qualified and effects_match
    if args.gpu_command_window:
        low,high=args.gpu_command_window
        if low<0 or high<=low or high-low>1000000:raise ValueError('invalid GPU command window')
        selected_env['PSX_SOURCE_GPU_COMMAND_WINDOW']=f'{low},{high}'
    if args.cpu_return_probe:
        from observation_evidence import validate_cpu_capture
        try:
            cpu_validation=validate_cpu_capture(run,ram_returns)
        except (ValueError,OSError) as error:
            cpu_validation={'valid':False,'error':str(error)}
        write_json(run/'cpu-capture-validation.json',cpu_validation)
        qualified=qualified and cpu_validation['valid']
    if args.ram_page_probe:
        from compare_ram_pages import validate_capture
        try:
            # The completion hook exits before the terminal return observer.
            ram_validation = validate_capture(run, identity['frames'] + args.neutral_tail - 1, args.ram_snapshot_frame)
        except (ValueError, OSError, StopIteration) as error:
            ram_validation = {'valid': False, 'error': str(error)}
        write_json(run / 'ram-capture-validation.json', ram_validation)
        qualified = qualified and ram_validation['valid']
    print(json.dumps({"input_playback_complete": qualified, "exit_code": code,
                      "gameplay_equivalence": "unclassified"}), flush=True)
    return 0 if qualified else 1


if __name__ == "__main__":
    raise SystemExit(main())
