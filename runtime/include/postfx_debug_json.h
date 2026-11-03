#pragma once
#include <stdio.h>
#include <string.h>
#include <math.h>
typedef struct PfxDebugLive { char name[32]; float v[4]; } PfxDebugLive;
typedef struct PfxJsonWriter { char *out; size_t cap,used; int ok; } PfxJsonWriter;
static inline void pfx_json_raw(PfxJsonWriter *w,const char *s) {
    const size_t n=strlen(s);
    if(!w->ok || n>=w->cap-w->used){w->ok=0;return;}
    memcpy(w->out+w->used,s,n+1);w->used+=n;
}
static inline void pfx_json_quote(PfxJsonWriter *w,const char *s) {
    pfx_json_raw(w,"\"");
    for(const unsigned char *p=(const unsigned char*)s;*p;++p) {
        char b[7];
        if(*p=='"'||*p=='\\'){b[0]='\\';b[1]=(char)*p;b[2]=0;}
        else if(*p<32)snprintf(b,sizeof b,"\\u%04x",(unsigned)*p);
        else {b[0]=(char)*p;b[1]=0;}
        pfx_json_raw(w,b);
    }
    pfx_json_raw(w,"\"");
}
/* Returns 0 with an empty buffer on insufficient space, never partial JSON. */
static inline int pfx_debug_json(char *out,int cap,const char *path,const char *status,
                                  const PfxDebugLive *live,int count) {
    if(!out||cap<=0)return 0;
    out[0]=0;PfxJsonWriter w={out,(size_t)cap,0,1};
    pfx_json_raw(&w,"\"path\":");pfx_json_quote(&w,path);
    pfx_json_raw(&w,",\"status\":");pfx_json_quote(&w,status);
    pfx_json_raw(&w,",\"uniforms\":{");
    for(int i=0;i<count;++i) {
        if(i)pfx_json_raw(&w,",");
        pfx_json_quote(&w,live[i].name);pfx_json_raw(&w,":[");
        for(int j=0;j<4;++j) {
            if(j)pfx_json_raw(&w,",");
            if(!isfinite(live[i].v[j])){w.ok=0;break;}
            char number[48];snprintf(number,sizeof number,"%.9g",(double)live[i].v[j]);pfx_json_raw(&w,number);
        }
        pfx_json_raw(&w,"]");
    }
    pfx_json_raw(&w,"}");
    if(!w.ok){out[0]=0;return 0;}return (int)w.used;
}
