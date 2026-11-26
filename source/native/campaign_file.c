#include "campaign_file.h"
#include <string.h>
FILE *campaign_file(const char *relative) {
    if(!relative || !*relative || relative[0]=='/' || strstr(relative,"..") || strchr(relative,':')) return NULL;
    const char *roots[]={"romfs:/campaign/","sdmc:/3ds/maxpayne/campaign/"};
    char path[192];
    for(unsigned i=0;i<sizeof roots/sizeof roots[0];i++) {
        if(snprintf(path,sizeof path,"%s%s",roots[i],relative)>=(int)sizeof path)return NULL;
        FILE *file=fopen(path,"rb");if(file)return file;
    }
    return NULL;
}
