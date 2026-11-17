#include "host_session_nonce.h"
#include <stdio.h>
#include <string.h>
int main(void){
    uint8_t a[16],b[16];memset(a,0,sizeof a);memset(b,0,sizeof b);
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    if(!psx_host_session_nonce(a)||!psx_host_session_nonce(b)||!memcmp(a,b,16))return 1;
    puts("fresh OS session nonces PASS");
#else
    if(psx_host_session_nonce(a))return 1;
    puts("unsupported OS fails closed for session nonce PASS");
#endif
    return 0;
}
