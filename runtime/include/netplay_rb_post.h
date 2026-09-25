#ifndef PSX_NETPLAY_RB_POST_H
#define PSX_NETPLAY_RB_POST_H

/*
 * MotK compatibility shim → recomp-net rb_post tip filter.
 */

#include "recomp_net/rb_post.h"

#ifdef __cplusplus
extern "C" {
#endif

#define netplay_rb_peer_post_tip_ok rnet_rb_peer_post_tip_ok

#ifdef __cplusplus
}
#endif

#endif /* PSX_NETPLAY_RB_POST_H */
