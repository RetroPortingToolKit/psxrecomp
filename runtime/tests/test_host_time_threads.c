#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "host_time.h"

static HANDLE start_event;
static volatile LONG errors;
static DWORD WINAPI sleeper(void *arg) {
    (void)arg;
    WaitForSingleObject(start_event, INFINITE);
    uint64_t previous = psx_host_mono_ms();
    for (unsigned i=0; i<250; ++i) {
        psx_host_sleep_micros(1000);
        uint64_t now = psx_host_mono_ms();
        if (now < previous) InterlockedIncrement(&errors);
        previous = now;
    }
    return 0;
}
int main(void) {
    HANDLE threads[8];
    DWORD before, after;
    if (!GetProcessHandleCount(GetCurrentProcess(), &before)) return 2;
    start_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!start_event) return 2;
    for (unsigned i=0; i<8; ++i) {
        threads[i] = CreateThread(NULL, 0, sleeper, NULL, 0, NULL);
        if (!threads[i]) return 2;
    }
    SetEvent(start_event);
    DWORD result = WaitForMultipleObjects(8, threads, TRUE, 10000);
    if (result != WAIT_OBJECT_0) {
        puts("FAIL: concurrent sleepers lost a wakeup");
        return 1;
    }
    for (unsigned i=0; i<8; ++i) CloseHandle(threads[i]);
    CloseHandle(start_event);
    if (!GetProcessHandleCount(GetCurrentProcess(), &after) || after > before) {
        puts("FAIL: sleeper timer handles leaked after thread exit");
        return 1;
    }
    if (errors) return 1;
    puts("host time concurrent sleepers: PASS");
    return 0;
}
