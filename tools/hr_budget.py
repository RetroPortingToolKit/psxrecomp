"""Bucket every host-sampler sample of one hr_run.py window (headroom-20261112).

py -3 hr_budget.py RUN.json [--nm C:/msys64/mingw64/bin/nm.exe] [--top 40] [--rules-out F]
Reads RUN.json (written by hr_run.py) and RUN.samples.bin (legacy16-byte or schema2/24-byte HostSample with
last-entered guest function and exception context, rva = pc - exe base, 0 when pc < exe base). Samples are 1 kHz wall-clock
samples of the EMULATION thread only (host_sampler.c); a blocked thread samples as its
wait site. Every sample lands in exactly one bucket; unattributed ones are counted.
"""
import argparse, bisect, json, re, struct, subprocess, sys
from collections import Counter
from pathlib import Path

RULES = [  # (bucket, regex on exe symbol) - first match wins; order matters
    ('debug_only', r'^(gpu_vram_peek|gr_vram_read|sw_vram_read|present_image_ring|debug_server|handle_|host_sampler|gcp_|wtrace|mmio_ring|ds_record|fntrace_ring|headless_present_image_ring|frame_fingerprint_|fp_ring|phase_sampler)'),
    ('static_bios_generated', r'^(SCPH1001_|OpenBIOS_)(func|alias|body)'),
    ('static_game_generated', r'^(func_[0-9A-Fa-f]{8}|psx_alias_body_|SCUS_|game_func_|psx_func_[0-9A-Fa-f]{8})'),
    ('pgxp_hooks', r'(^pgxp|^psx_pgxp|PGXP|_pgxp|precise_triangle|parse_vertex|note_store|alu_bitwise)'),
    ('interpreter', r'(dirty_ram_interp|^interp_|_interp_|^psx_interp|^cpu_interp|^dri_|interpret)'),
    ('block_edge_irq_check', r'^(overlay_cpu_charge|overlay_ci_at|psx_slice_block|psx_check_interrupts|psx_cyc_bb_defer|psx_cyc_batch_flush|psx_idle_note|idle_note|poll_staged_user_snapshot|psx_netplay_active|savestate_poll|psx_native_wait|native_wait)'),
    ('trampoline_dispatch', r'(dispatch|text_native_ok|^fntrace_|^ls_func_|psx_game_text_native_ok|mod_runtime_on_dispatch|^psx_call|trampoline|^overlay_call|^overlay_lookup|cps_)'),
    ('memory_load_store', r'^(psx_native_span|ls_at_leader|psx_lwl|psx_lwr|psx_swl|psx_swr|psx_mod_read|psx_read_|psx_write_|psx_cyc_load|psx_store_cycle_barrier|ls_read_hook|ls_write|memory_|psx_mem|ram_|scratch|psx_ram|cache_ctrl|psx_load_|psx_store_)'),
    ('gte', r'(^gte_|gte_|GTE|cop2)'),
    ('widescreen_mod', r'^(ws_|psx_ws_|gpu_ws_)'),
    ('gp0_gpu_command', r'^(gp0|gp1|gpu_(write|read|gp0|gp1|cmd|dma|status|process|exec)|psx_gpu|gpu_ot|gpu_linked|gpustat)'),
    ('renderer_backend', r'(texel_fetch|^gl_|gpu_gl|^sw_|raster|^draw_|^vk_|gpu_vk|renderer|^hr_|texture|tex_cache|vram|^blit|^fill_|^poly|^line_|^rect_|^span_|shade|^gpu_)'),
    ('spu_audio', r'^(spu_|psx_spu|reverb|rev_|adpcm|xa_|sdl_audio|audio_|cdda|process_cdda)'),
    ('scheduler_devices', r'^(psx_icache_fetch|psx_cpu_charge|psx_get_cycle_count|psx_devices_|psx_cycle|interrupts_|timer|root_counter|rcnt|dma_|cdrom_|cd_|sio_|mdec_|psx_cyc_|psx_timer|psx_dma|psx_cdrom|psx_sio|vblank|hblank|psx_irq|irq_|exception|psx_exception|cop0)'),
    ('frame_end', r'(^sdl_|present|^frame_|rewind|snap_|savestate|netplay|^input|^pad_|mod_call_frame_hooks|finish_frame|^psx_frame|^main_loop|end_of_frame|^mod_|widescreen|refresh_)'),
    ('crt_libc', r'^(ldexp|floor|___chkstk|memcpy|memset|memmove|memcmp|_?_?mingw|__chk|strlen|malloc|free)'),
]
SYS_MOD = [  # bucket for non-exe modules by file name
    ('gl_driver', r'(nvoglv|opengl32|nvgl|atio|ig\w+icd)'),
    ('os_wait_sync', r'^(ntdll|kernelbase|kernel32|win32u|user32|gdi32|combase|rpcrt4|sechost|advapi32|ws2_32|mswsock)\.dll$'),
    ('crt_libc', r'^(ucrtbase|msvcrt|libstdc\+\+|libgcc|libwinpthread|vcruntime)'),
    ('sdl', r'^sdl'),
    ('audio_stack', r'(audio|mmdev|dsound|winmm|avrt)'),
]


def nm_table(nm, exe):
    out = subprocess.run([nm, '--defined-only', str(exe)], capture_output=True, text=True, check=True).stdout
    head = Path(exe).read_bytes()[:4096]  # PE32+ ImageBase (same as tools/host_profile.py)
    pe = int.from_bytes(head[0x3C:0x40], 'little')
    base = int.from_bytes(head[pe + 24 + 24:pe + 24 + 32], 'little'); tab = []
    for line in out.splitlines():
        p = line.split(None, 2)
        if len(p) == 3 and p[1] in ('T', 't'):
            tab.append((int(p[0], 16) - base, p[2]))
    tab.sort()
    return tab


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--nm', default='C:/msys64/mingw64/bin/nm.exe')
    ap.add_argument('--top', type=int, default=40)
    a = ap.parse_args()
    run = json.loads(Path(a.run).read_text(encoding='utf-8'))
    exe = Path(run['exe'])
    raw = Path(a.run[:-5] + '.samples.bin').read_bytes()
    record_bytes = run.get('sampler', {}).get('record_bytes', 16)
    if record_bytes not in (16, 24) or len(raw) % record_bytes:
        raise ValueError('unsupported or truncated host sample records')
    n = len(raw) // record_bytes
    samples = [struct.unpack_from('<QII', raw, i * record_bytes) for i in range(n)]
    guest_samples = Counter()
    guest_context = []
    guest_buckets = Counter()
    if record_bytes == 24:
        for i in range(n):
            pc, flags = struct.unpack_from('<II', raw, i * record_bytes + 16)
            guest_context.append((pc, bool(flags & 1)))
            guest_samples[(pc, bool(flags & 1))] += 1
    tab = nm_table(a.nm, exe); keys = [x[0] for x in tab]
    mods = run['modules']
    exe_mod = next(m for m in mods if Path(m['path']).name.lower() == exe.name.lower())
    exe_base = exe_mod['base']; exe_size = exe_mod['size']
    other = sorted((m['base'], m['base'] + m['size'], m['path']) for m in mods if m is not exe_mod)
    obase = [o[0] for o in other]
    sym_ct = Counter(); bucket_ct = Counter(); mod_ct = Counter(); passes = 0
    def note_bucket(bucket, index):
        bucket_ct[bucket] += 1
        if guest_context:
            pc, exception = guest_context[index]
            guest_buckets[(pc, exception, bucket)] += 1
    for sample_index, (rva, frame, in_pass) in enumerate(samples):
        passes += in_pass
        if rva == 0:
            note_bucket('unattributed_nonimage', sample_index); sym_ct['(pc below exe base)'] += 1; continue
        if rva < exe_size:
            i = bisect.bisect_right(keys, rva) - 1
            name = tab[i][1] if i >= 0 else '(exe, no symbol)'
            sym_ct[name] += 1
            b = next((bk for bk, rx in RULES if re.search(rx, name)), 'exe_other')
            note_bucket(b, sample_index)
            continue
        pc = exe_base + rva
        j = bisect.bisect_right(obase, pc) - 1
        if j >= 0 and pc < other[j][1]:
            path = other[j][2]; fn = Path(path).name.lower()
            mod_ct[path] += 1
            if path.lower().startswith(str(exe.parent).lower()) and fn.endswith('.dll') and fn != 'sdl3.dll':
                b = 'overlay_dll_code'
            else:
                b = next((bk for bk, rx in SYS_MOD if re.search(rx, fn)), 'other_module')
            note_bucket(b, sample_index); sym_ct['[%s]' % Path(path).name] += 1
        else:
            note_bucket('unattributed_unknown_pc', sample_index); sym_ct['(unknown pc)'] += 1
    frames = run['f1'] - run['f0']
    wall_us_per_frame = 1e6 * run['wall_s'] / frames
    total = sum(bucket_ct.values())
    res = {'label': run['label'], 'samples': total, 'frames': frames, 'wall_s': run['wall_s'],
           'sample_rate_hz': total / run['wall_s'], 'in_render_pass': passes,
           'guest_pc_kind': run.get('sampler', {}).get('guest_pc_kind', 'unavailable'),
           'guest_functions': [{'pc': '0x%08X' % pc, 'exception': exc, 'samples': count}
                               for (pc, exc), count in guest_samples.most_common()],
           'guest_buckets': [{'pc': '0x%08X' % pc, 'exception': exc, 'bucket': bucket, 'samples': count,
                              'sampled_wall_us_per_frame': wall_us_per_frame * count / total}
                             for (pc, exc, bucket), count in guest_buckets.most_common()],
           'us_per_frame_basis': 'bucket share x wall us/frame (%.1f); sampler is 1 kHz wall-clock on emu thread' % wall_us_per_frame,
           'buckets': [[b, c, 100.0 * c / total, wall_us_per_frame * c / total] for b, c in bucket_ct.most_common()],
           'top_symbols': [[s, c, 100.0 * c / total,
                            next((bk for bk, rx in RULES if re.search(rx, s)), 'module/exe_other')] for s, c in sym_ct.most_common(a.top)],
           'modules': [[Path(p).name, c] for p, c in mod_ct.most_common(30)]}
    Path(a.run[:-5] + '.budget.json').write_text(json.dumps(res, indent=1), encoding='utf-8')
    print('%s samples=%d frames=%d wall_us/frame=%.1f rate=%.0fHz' % (res['label'], total, frames, wall_us_per_frame, res['sample_rate_hz']))
    for b, c, pct, us in res['buckets']:
        print('  %-26s %7d %6.2f%% %8.1f us/frame' % (b, c, pct, us))
    print('top symbols:')
    for s, c, pct, b in res['top_symbols']:
        print('  %6d %5.2f%%  %-22s %s' % (c, pct, b, s[:90]))
    print('modules:', res['modules'][:15])


if __name__ == '__main__':
    main()
