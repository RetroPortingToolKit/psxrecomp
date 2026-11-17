#include "host_session_nonce.h"
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif defined(__linux__)
#include <sys/random.h>
#include <errno.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <stdlib.h>
#endif
int psx_host_session_nonce(uint8_t out[16]){
    uint8_t bytes[16];if(!out)return 0;
#if defined(_WIN32)
    if(BCryptGenRandom(NULL,bytes,sizeof bytes,BCRYPT_USE_SYSTEM_PREFERRED_RNG)!=0)return 0;
#elif defined(__linux__)
    size_t at=0;while(at<sizeof bytes){ssize_t n=getrandom(bytes+at,sizeof bytes-at,0);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)return 0;
        at+=(size_t)n;}
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    arc4random_buf(bytes,sizeof bytes);
#else
    return 0;
#endif
    unsigned any=0;for(unsigned i=0;i<sizeof bytes;++i)any|=bytes[i];
    if(!any)return 0;
    memcpy(out,bytes,sizeof bytes);return 1;
}
