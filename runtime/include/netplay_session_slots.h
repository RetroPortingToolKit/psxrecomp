/* netplay_session_slots.h - how many session slots a netplay peer runs with.
 *
 * Every peer must arrive at the same count, or their input bundles and
 * rollback state disagree. A spectator's lobby seat lives in the gallery
 * (spectator_slot_base and up, e.g. 64), not in the player table, so it must
 * not widen the count the way a player's seat does: before this, a spectator
 * in a 2-player room ran with the slot cap (4) while the players ran with 2,
 * and its relay slot (relay base 2) then failed the "above every player
 * seat" check and the spectator refused to start. */
#ifndef PSX_NETPLAY_SESSION_SLOTS_H
#define PSX_NETPLAY_SESSION_SLOTS_H

static inline int psx_netplay_session_slot_count(int player_count, int max_slots,
                                                 int local_slot, int is_spectator,
                                                 int game_fallback, int slot_cap)
{
    int slots = player_count >= 2 ? player_count
              : (max_slots >= 2 ? max_slots
                 : (game_fallback >= 2 ? game_fallback : 2));
    if (!is_spectator && local_slot + 1 > slots) slots = local_slot + 1;
    if (slots < 2) slots = 2;
    if (slots > slot_cap) slots = slot_cap;
    return slots;
}

#endif
