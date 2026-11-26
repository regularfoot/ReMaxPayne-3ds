#include "localization.h"
#include "campaign_file.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {const char *key,*text;} Caption;
static Caption *captions;
static char *bytes;
static unsigned count;
void localization_close(void){free(captions);free(bytes);captions=NULL;bytes=NULL;count=0;}
bool localization_open(void){
    localization_close();FILE *f=campaign_file("texts.bin");if(!f)return false;
    struct {char tag[4];uint32_t count,bytes;} h;
    bool ok=fread(&h,sizeof h,1,f)==1 && !memcmp(h.tag,"MPT1",4) && h.count && h.count<=4096 && h.bytes && h.bytes<=512*1024;
    if(ok){
        bytes=malloc(h.bytes);captions=calloc(h.count,sizeof *captions);
        ok=bytes && captions && fread(bytes,1,h.bytes,f)==h.bytes && fgetc(f)==EOF;
    }
    fclose(f);
    if(ok){
        char *p=bytes,*end=bytes+h.bytes;
        for(unsigned i=0;i<h.count && ok;i++){
            char *key=p,*zero=memchr(p,0,end-p);
            ok=zero && zero>p && zero-p<=100;
            if(!ok)break;
            ok=strspn(key,"abcdefghijklmnopqrstuvwxyz0123456789_-")==(size_t)(zero-key);
            p=zero+1;zero=memchr(p,0,end-p);
            if(!ok || !zero){ok=false;break;}
            captions[i]=(Caption){key,p};p=zero+1;
            if(i && strcmp(captions[i-1].key,key)>=0)ok=false;
        }
        ok=ok && p==end;
    }
    if(!ok){localization_close();return false;}
    count=h.count;return true;
}
void localization_read(const char *key,char *out,size_t capacity){
    if(!capacity)return;
    *out=0;unsigned lo=0,hi=count;
    while(lo<hi){unsigned mid=(lo+hi)/2;if(strcmp(captions[mid].key,key)<0)lo=mid+1;else hi=mid;}
    if(lo==count || strcmp(captions[lo].key,key))return;
    size_t n=strlen(captions[lo].text);if(n>=capacity)n=capacity-1;
    memcpy(out,captions[lo].text,n);out[n]=0;
}
