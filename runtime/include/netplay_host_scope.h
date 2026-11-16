#ifndef PSX_NETPLAY_HOST_SCOPE_H
#define PSX_NETPLAY_HOST_SCOPE_H
#include <string.h>
static int np_host_numeric_loopback(const char *endpoint){
    if(!endpoint||strncmp(endpoint,"127.0.0.1:",10))return 0;
    const char *p=endpoint+10;unsigned port=0,count=0;
    while(*p){if(*p<'0'||*p>'9'||++count>5)return 0;port=port*10+(unsigned)(*p++-'0');}
    return count&&port&&port<=65535;
}
static int np_host_direct_loopback_scope(int resolved_transport,int lobby,int routed,
                                         const char *bind,const char *peer){
    return resolved_transport==0&&!lobby&&!routed&&np_host_numeric_loopback(bind)&&
        peer&&(!peer[0]||np_host_numeric_loopback(peer));
}
#endif
