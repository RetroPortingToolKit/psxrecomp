/* No TAS route is installed in hardware-only SIO fixtures. Match the
 * production callback's disabled-route behavior; never alter pad input. */
#include <stdint.h>
uint16_t debug_server_update_poll(int slot, uint16_t buttons, int analog) {
    (void)slot; (void)analog; return buttons;
}
