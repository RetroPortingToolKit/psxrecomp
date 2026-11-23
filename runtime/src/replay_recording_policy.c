#include "replay_frontend.h"

static int requested;
void replay_recording_request(int enabled) { requested = enabled != 0; }
int replay_recording_requested(void) { return requested; }
