#include "openxr_ready_state.h"

#include <stdio.h>

#define CHECK(v, m) do { if (!(v)) { \
    fprintf(stderr, "FAIL: %s\n", (m)); return 1; } } while (0)

int main(void) {
    PSXOpenXRReadyLoadState state = {0};
    PSXOpenXRReadyLoadRequest request = {0};
    PSXOpenXRReadyLoadStatus status = {0};

    psx_openxr_ready_load_begin_execution(&state, 1, 0, -1);
    CHECK(psx_openxr_ready_load_ok(&state), "no requested load is ready");

    psx_openxr_ready_load_begin_execution(&state, 1, 1, 2);
    CHECK(!psx_openxr_ready_load_ok(&state), "requested load waits for its request");

    request = (PSXOpenXRReadyLoadRequest){1, 1, 10, 2, NULL};
    CHECK(psx_openxr_ready_load_observe_request(&state, &request),
          "first load request is accepted");
    CHECK(!psx_openxr_ready_load_ok(&state), "pending request is not ready");
    status = (PSXOpenXRReadyLoadStatus){10, 1, 0, 1, 2};
    CHECK(!psx_openxr_ready_load_observe_status(&state, &status),
          "pending request cannot resolve");
    status = (PSXOpenXRReadyLoadStatus){11, 0, 1, 1, 2};
    CHECK(psx_openxr_ready_load_observe_status(&state, &status),
          "completed current request resolves");
    CHECK(psx_openxr_ready_load_ok(&state), "successful load is ready");

    /* An XR session re-init in the same execution rebinds the current request. */
    psx_openxr_ready_load_begin_execution(&state, 1, 1, 2);
    CHECK(!psx_openxr_ready_load_ok(&state), "session re-init clears readiness");
    request = (PSXOpenXRReadyLoadRequest){1, 1, 10, 2, NULL};
    CHECK(psx_openxr_ready_load_observe_request(&state, &request),
          "current execution request is rebound");
    status = (PSXOpenXRReadyLoadStatus){11, 0, 1, 1, 2};
    CHECK(psx_openxr_ready_load_observe_status(&state, &status) &&
          psx_openxr_ready_load_ok(&state),
          "current execution stays ready after XR session re-init");

    /* A reboot must ignore both the prior status and the prior request. */
    psx_openxr_ready_load_begin_execution(&state, 2, 1, 2);
    status = (PSXOpenXRReadyLoadStatus){11, 0, 1, 1, 2};
    CHECK(!psx_openxr_ready_load_observe_status(&state, &status),
          "previous session success cannot authorize reboot");
    CHECK(!psx_openxr_ready_load_ok(&state), "reboot waits for a fresh load");
    request = (PSXOpenXRReadyLoadRequest){1, 1, 10, 2, NULL};
    CHECK(!psx_openxr_ready_load_observe_request(&state, &request),
          "stale execution request is ignored");

    request = (PSXOpenXRReadyLoadRequest){2, 2, 11, 2, NULL};
    CHECK(psx_openxr_ready_load_observe_request(&state, &request),
          "reboot load request is accepted");
    CHECK(!psx_openxr_ready_load_ok(&state),
          "reboot pending load cannot reuse the prior success");
    status = (PSXOpenXRReadyLoadStatus){12, 0, 0, 1, 2};
    CHECK(psx_openxr_ready_load_observe_status(&state, &status),
          "reboot failed load resolves as failure");
    CHECK(psx_openxr_ready_load_failed(&state),
          "reboot failed load cannot authorize success");

    psx_openxr_ready_load_begin_execution(&state, 3, 1, 2);
    request = (PSXOpenXRReadyLoadRequest){3, 3, 12, 2, NULL};
    CHECK(psx_openxr_ready_load_observe_request(&state, &request),
          "recovery load request is accepted");
    status = (PSXOpenXRReadyLoadStatus){13, 0, 1, 1, 2};
    CHECK(psx_openxr_ready_load_observe_status(&state, &status),
          "recovery load resolves");
    CHECK(psx_openxr_ready_load_ok(&state), "recovery session is ready");

    psx_openxr_ready_load_begin_execution(&state, 4, 1, 3);
    request = (PSXOpenXRReadyLoadRequest){4, 4, 13, -1,
        "state_load_request_refused"};
    CHECK(psx_openxr_ready_load_observe_request(&state, &request),
          "refused load request is accepted");
    CHECK(psx_openxr_ready_load_failed(&state) &&
          state.error && state.error[0],
          "refused load reports failure in its execution");

    puts("ALL PASS");
    return 0;
}
