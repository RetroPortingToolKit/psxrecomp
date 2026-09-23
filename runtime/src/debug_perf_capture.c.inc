/* Included by debug_server.c: emulation-thread-only, opt-in, bounded samples.
 * No framebuffer readback, guest execution, file IO or per-primitive timers.
 * RAM counter/state addresses are supplied by the client, never title-specific.
 * Wall intervals are frame-boundary intervals, not isolated CPU/GPU busy time. */
#define PERF_CAPTURE_CAP 16384
typedef struct {
    uint64_t ticks, cycles, frame, batches;
    uint32_t draw, state;
} PerfCaptureSample;
static PerfCaptureSample *s_perf_capture;
static uint32_t s_perf_count, s_perf_draw_addr, s_perf_state_addr;
static uint32_t s_perf_draw_limit, s_perf_stop_state, s_perf_warmup;
static int s_perf_active, s_perf_until_state, s_perf_read_state;
static const char *s_perf_end = "idle";
static uint64_t s_perf_frequency;
static uint64_t s_perf_work_start[8], s_perf_work_end[8];
static double s_perf_gl_start[3],s_perf_gl_end[3];
static uint64_t s_perf_batch_start[8],s_perf_batch_end[8];
static uint64_t s_perf_audio_start[2],s_perf_audio_end[2];
static int s_perf_audio_active;
static double s_perf_rewind_start[4],s_perf_rewind_end[4];

static void perf_audio_snapshot(uint64_t out[2]) {
    double fill,target,correction;int legacy,rate;
    s_perf_audio_active=psx_audio_out_stats(&fill,&target,&out[0],&out[1],
                                          &correction,&legacy,&rate);
}

static void perf_capture_snapshot_end(void);

static int perf_ram_address(uint32_t addr) {
    return !(addr & 3u) && (addr & 0x1fffffffu) < 0x200000u &&
        (addr < 0x200000u || (addr >= 0x80000000u && addr < 0x80200000u) ||
         (addr >= 0xa0000000u && addr < 0xa0200000u));
}

static int perf_parse_address(const char *text,uint32_t *address) {
    char *end;unsigned long long wide=strtoull(text,&end,16);
    if (end==text || *end || wide>UINT32_MAX) return 0;
    *address=(uint32_t)wide;return perf_ram_address(*address);
}

static void perf_work_snapshot(uint64_t out[8]) {
    uint32_t loads, invalidations, unregistered, revalidations;
    uint64_t native, interp, stale, load_us;
    overlay_loader_get_counters(&loads, &invalidations, &unregistered,
        &native, &interp, &stale, NULL, NULL, NULL, NULL, &revalidations);
    overlay_loader_get_load_timing(&load_us, NULL, NULL);
    out[0]=loads; out[1]=invalidations; out[2]=revalidations; out[3]=native;
    out[4]=interp; out[5]=stale; out[6]=load_us; out[7]=unregistered;
}

static void perf_capture_snapshot_end(void) {
    if (!s_perf_count) return;
    perf_work_snapshot(s_perf_work_end);
    gl_renderer_submission_diag(s_perf_gl_end);
    gl_renderer_batch_diag(s_perf_batch_end);
    perf_audio_snapshot(s_perf_audio_end);
    psx_rewind_perf(s_perf_rewind_end);
}

static void perf_capture_tick(void) {
    if (!s_perf_active) return;
    if (s_perf_warmup) { --s_perf_warmup; return; }
    PerfCaptureSample *s = &s_perf_capture[s_perf_count];
    s->ticks = SDL_GetPerformanceCounter();
    s->cycles = psx_get_cycle_count();
    s->frame = s_frame_count;
    s->draw = psx_read_word(s_perf_draw_addr);
    s->state = s_perf_read_state ? psx_read_word(s_perf_state_addr) : 0;
    uint64_t batches[8]; gl_renderer_batch_diag(batches); s->batches=batches[0];
    if (!s_perf_count) {
        perf_work_snapshot(s_perf_work_start);
        gl_renderer_submission_diag(s_perf_gl_start);
        gl_renderer_batch_diag(s_perf_batch_start);
        perf_audio_snapshot(s_perf_audio_start);
        psx_rewind_perf(s_perf_rewind_start);
        memcpy(s_perf_work_end,s_perf_work_start,sizeof(s_perf_work_end));
        memcpy(s_perf_gl_end,s_perf_gl_start,sizeof(s_perf_gl_end));
        memcpy(s_perf_batch_end,s_perf_batch_start,sizeof(s_perf_batch_end));
        memcpy(s_perf_audio_end,s_perf_audio_start,sizeof(s_perf_audio_end));
        memcpy(s_perf_rewind_end,s_perf_rewind_start,sizeof(s_perf_rewind_end));
    }
    ++s_perf_count;
    if ((s_perf_count==1 && s_perf_until_state && s->state==s_perf_stop_state) ||
        (uint32_t)(s->draw-s_perf_capture[0].draw)>8000 ||
        (s_perf_count>1 && ((uint32_t)(s->draw-s_perf_capture[s_perf_count-2].draw)>8000 ||
                           s->cycles<s_perf_capture[s_perf_count-2].cycles))) {
        s_perf_active=0; s_perf_end="invalid_counter_or_start";
    } else if (s_perf_count > 1 &&
        ((s_perf_draw_limit && (uint32_t)(s->draw-s_perf_capture[0].draw)>=s_perf_draw_limit) ||
         (s_perf_until_state && s->state==s_perf_stop_state))) {
        s_perf_active=0; s_perf_end="complete";
    } else if (s_perf_count == PERF_CAPTURE_CAP) {
        s_perf_active=0; s_perf_end="capacity";
    }
    if (!s_perf_active) {
        perf_capture_snapshot_end();
    }
}

static void handle_perf_capture(int id, const char *json) {
    char op[24]="status", address[24];
    json_get_str(json,"op",op,sizeof(op));
    if (!strcmp(op,"start")) {
        if (s_perf_active) { send_err(id,"capture is active"); return; }
        if (!json_get_str(json,"draw_addr",address,sizeof(address))) {
            send_err(id,"missing RAM draw_addr"); return;
        }
        uint32_t draw_addr=0, state_addr=0;
        if (!perf_parse_address(address,&draw_addr)) { send_err(id,"invalid RAM draw_addr");return; }
        int until_state=json_get_str(json,"state_addr",address,sizeof(address)) != NULL;
        int read_state=until_state || json_get_str(json,"aux_addr",address,sizeof(address));
        if (read_state &&
            !perf_parse_address(address,&state_addr)) { send_err(id,"invalid RAM state/aux address");return; }
        int draws=json_get_int(json,"draws",0), warmup=json_get_int(json,"warmup",8);
        if (!perf_ram_address(draw_addr) || (read_state && !perf_ram_address(state_addr)) ||
            draws<0 || draws>8000 || (!draws && !until_state) || warmup<0 || warmup>600) {
            send_err(id,"invalid RAM address or capture limits"); return;
        }
        if (!s_perf_capture) s_perf_capture=calloc(PERF_CAPTURE_CAP,sizeof(*s_perf_capture));
        if (!s_perf_capture) { send_err(id,"capture allocation failed"); return; }
        s_perf_draw_addr=draw_addr; s_perf_state_addr=state_addr;
        s_perf_until_state=until_state; s_perf_draw_limit=(uint32_t)draws;
        s_perf_read_state=read_state;
        s_perf_stop_state=(uint32_t)json_get_int(json,"state",0);
        s_perf_warmup=(uint32_t)warmup; s_perf_count=0;
        memset(s_perf_work_start,0,sizeof(s_perf_work_start));
        memset(s_perf_work_end,0,sizeof(s_perf_work_end));
        memset(s_perf_gl_start,0,sizeof(s_perf_gl_start));
        memset(s_perf_gl_end,0,sizeof(s_perf_gl_end));
        memset(s_perf_batch_start,0,sizeof(s_perf_batch_start));
        memset(s_perf_batch_end,0,sizeof(s_perf_batch_end));
        memset(s_perf_audio_start,0,sizeof(s_perf_audio_start));
        memset(s_perf_audio_end,0,sizeof(s_perf_audio_end));
        s_perf_audio_active=0;
        memset(s_perf_rewind_start,0,sizeof(s_perf_rewind_start));
        memset(s_perf_rewind_end,0,sizeof(s_perf_rewind_end));
        s_perf_frequency=SDL_GetPerformanceFrequency();
        s_perf_active=1; s_perf_end="running";
        send_ok(id); return;
    }
    if (!strcmp(op,"stop")) {
        if (s_perf_active) { perf_capture_snapshot_end(); s_perf_end="stopped"; }
        s_perf_active=0;
    } else if (!strcmp(op,"read")) {
        if (s_perf_active) { send_err(id,"stop capture before reading"); return; }
        int offset=json_get_int(json,"offset",0), count=json_get_int(json,"count",128);
        if (offset<0 || count<1 || count>128) { send_err(id,"invalid sample range"); return; }
        char response[32768]; int used=snprintf(response,sizeof(response),
            "{\"id\":%d,\"ok\":true,\"samples\":[",id);
        for (uint32_t i=(uint32_t)offset; i<s_perf_count && i<(uint32_t)offset+(uint32_t)count; ++i) {
            const PerfCaptureSample *s=&s_perf_capture[i];
            used+=snprintf(response+used,sizeof(response)-(size_t)used,
                "%s[%llu,%llu,%llu,%u,%u,%llu]",i==(uint32_t)offset?"":",",
                (unsigned long long)s->ticks,(unsigned long long)s->cycles,
                (unsigned long long)s->frame,s->draw,s->state,(unsigned long long)s->batches);
        }
        snprintf(response+used,sizeof(response)-(size_t)used,"]}"); send_line(response); return;
    } else if (strcmp(op,"status")) { send_err(id,"unknown capture operation"); return; }
    if (s_perf_active) perf_capture_snapshot_end();
    send_fmt("{\"id\":%d,\"ok\":true,\"active\":%d,\"end\":\"%s\",\"count\":%u,"
        "\"frequency\":%llu,\"scale\":%d,\"gl_cpu_ms\":[%.3f,%.3f],"
        "\"audio\":{\"active\":%d,\"underruns\":%llu,\"overflow_drops\":%llu},"
        "\"rewind\":[%.0f,%.3f,%.3f,%.3f],"
        "\"batch_delta\":[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu],"
        "\"overlay_delta\":[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu]}",
        id,s_perf_active,s_perf_end,s_perf_count,(unsigned long long)s_perf_frequency,gr_scale(),
        s_perf_gl_end[0]-s_perf_gl_start[0],s_perf_gl_end[1]-s_perf_gl_start[1],
        s_perf_audio_active,(unsigned long long)(s_perf_audio_end[0]-s_perf_audio_start[0]),
        (unsigned long long)(s_perf_audio_end[1]-s_perf_audio_start[1]),
        s_perf_rewind_end[0]-s_perf_rewind_start[0],s_perf_rewind_end[1]-s_perf_rewind_start[1],
        s_perf_rewind_end[2]-s_perf_rewind_start[2],s_perf_rewind_end[3]-s_perf_rewind_start[3],
        (unsigned long long)(s_perf_batch_end[0]-s_perf_batch_start[0]),
        (unsigned long long)(s_perf_batch_end[1]-s_perf_batch_start[1]),
        (unsigned long long)(s_perf_batch_end[2]-s_perf_batch_start[2]),
        (unsigned long long)(s_perf_batch_end[3]-s_perf_batch_start[3]),
        (unsigned long long)(s_perf_batch_end[4]-s_perf_batch_start[4]),
        (unsigned long long)(s_perf_batch_end[5]-s_perf_batch_start[5]),
        (unsigned long long)(s_perf_batch_end[6]-s_perf_batch_start[6]),
        (unsigned long long)(s_perf_batch_end[7]-s_perf_batch_start[7]),
        (unsigned long long)(s_perf_work_end[0]-s_perf_work_start[0]),
        (unsigned long long)(s_perf_work_end[1]-s_perf_work_start[1]),
        (unsigned long long)(s_perf_work_end[2]-s_perf_work_start[2]),
        (unsigned long long)(s_perf_work_end[3]-s_perf_work_start[3]),
        (unsigned long long)(s_perf_work_end[4]-s_perf_work_start[4]),
        (unsigned long long)(s_perf_work_end[5]-s_perf_work_start[5]),
        (unsigned long long)(s_perf_work_end[6]-s_perf_work_start[6]),
        (unsigned long long)(s_perf_work_end[7]-s_perf_work_start[7]));
}
