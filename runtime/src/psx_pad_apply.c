/* The single PsxNetPad -> SIO path (see psx_netplay.h). */
#include "psx_netplay.h"
#include "sio.h"

void psx_pad_apply_to_sio(int port, const PsxNetPad *pad)
{
    if (!pad || port < 0 || port >= PSX_MAX_PLAYERS) return;
    if (sio_pad_on_multitap(port) && !sio_get_multitap_analog())
        sio_set_pad_config_capable(port, 0);
    sio_set_pad_state_slot(port, pad->buttons);
    if (pad->analog == SIO_PAD_NEGCON) {
        sio_set_pad_sticks(port, pad->lx, 0x80, 0x80, 0x80);
        sio_set_pad_negcon(port, pad->rx, pad->ry, pad->ly);
    } else {
        sio_set_pad_sticks(port, pad->lx, pad->ly, pad->rx, pad->ry);
        sio_set_pad_negcon(port, 0, 0, 0);
    }
    sio_request_pad_type(port, pad->analog <= PSX_NETPAD_TYPE_MAX
                                   ? pad->analog : SIO_PAD_DIGITAL);
}
