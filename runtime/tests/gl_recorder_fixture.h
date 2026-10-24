#pragma once
/* Renderer fixtures have no encoder session. Recorder lifecycle and real encoding
 * are covered by video_recorder_*_test. */
#include "video_recorder.h"
int video_recorder_present_poll(uint32_t *session) {(void)session;return VREC_PRESENT_IDLE;}
int video_recorder_flush_wanted(uint32_t *session) {(void)session;return 0;}
void video_recorder_flush_done(uint32_t session) {(void)session;}
void video_recorder_note_lost_frame(uint32_t session) {(void)session;}
void video_recorder_submit_frame(uint32_t session,const void *pixels,int w,int h,int stride,int bottom_up,uint64_t cycle) {
    (void)session;(void)pixels;(void)w;(void)h;(void)stride;(void)bottom_up;(void)cycle;
}
