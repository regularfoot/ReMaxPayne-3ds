#include "localization.h"
#include "campaign_file.h"
#include <assert.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static unsigned opens;
static bool corrupt;
FILE *campaign_file(const char *name){
    opens++;assert(!strcmp(name,"texts.bin"));
    if(!corrupt)return fopen(name,"rb");
    FILE *f=tmpfile();assert(f);
    struct {char tag[4];uint32_t count,bytes;} h={{'M','P','T','1'},2,4};
    assert(fwrite(&h,sizeof h,1,f)==1 && fwrite("a\0x\0",4,1,f)==1);rewind(f);return f;
}
int main(void){
    assert(localization_open() && opens==1);
    DIR *dir=opendir("objectives");assert(dir);struct dirent *entry;unsigned checked=0;
    while((entry=readdir(dir))){
        size_t n=strlen(entry->d_name);if(n<=4 || strcmp(entry->d_name+n-4,".txt"))continue;
        char key[128],path[160],expected[512]={0},actual[512];
        assert(n-4<sizeof key);memcpy(key,entry->d_name,n-4);key[n-4]=0;
        snprintf(path,sizeof path,"objectives/%s",entry->d_name);
        FILE *f=fopen(path,"rb");assert(f);assert(fgets(expected,sizeof expected,f));fclose(f);
        char *newline=strchr(expected,'\n');if(newline)*newline=0;
        localization_read(key,actual,sizeof actual);assert(!strcmp(expected,actual));
        localization_read(key,actual,4);
        size_t length=strlen(expected);
        assert(!strncmp(expected,actual,3) && actual[length<3?length:3]==0);
        localization_read(key,actual,1);assert(actual[0]==0);checked++;
    }
    closedir(dir);assert(checked==199 && opens==1);
    char out[192];localization_read("tip_02",out,sizeof out);assert(*out);
    const char *key=localization_key_for_saved_text(out,sizeof out);
    assert(key && !strcmp(key,"tip_02"));
    assert(!localization_key_for_saved_text("",sizeof out));
    assert(!localization_key_for_saved_text("Unrecognized legacy caption",sizeof out));
    localization_read("../../unknown",out,sizeof out);assert(!*out);localization_close();
    localization_read("tip_02",out,sizeof out);assert(!*out);
    corrupt=true;assert(!localization_open());localization_close();
    printf("All %u HUD captions match original files; lookup performs no file opens; malformed bank rejected\n",checked);return 0;
}
