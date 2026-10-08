#ifndef PSXRECOMP_MOD_LOCAL_INPUT_POLICY_H
#define PSXRECOMP_MOD_LOCAL_INPUT_POLICY_H

/* Local presentation plugins may inspect live host input only while its
 * controller is available during ordinary offline execution. Guest pad mode
 * is deliberately absent: a game selecting digital SIO does not make its
 * locally attached analog stick unavailable to presentation code. Peer input
 * and rollback replay remain simulation state, never presentation input. */
static inline int psx_mod_local_input_available(int player_valid,
                                                int connected,
                                                int netplay_active,
                                                int resimulating) {
    return player_valid && connected &&
           !netplay_active && !resimulating;
}

#endif
