/* Language selection must not replace records held by active/queued voices. */
#include <assert.h>
#include "../source/native/audio_bank.c"
Director director;
UiSettings ui_settings={.volume=4,.music=3};
FILE *campaign_file(const char *name){return fopen(name,"rb");}
void director_pose(unsigned id,Point *p,Point *forward,Point *up){(void)id;(void)forward;(void)up;if(p)*p=(Point){0};}
bool campaign_sound_path(unsigned room,Point ear,unsigned source_room,Point source,const bool *enabled,Point *apparent,float *distance){
    (void)room;(void)ear;(void)source_room;(void)enabled;*apparent=source;*distance=0;return true;
}
int main(void){
    static Record clips[]={
        {.key="dialog/line",.at=100,.bytes=32,.gain=1,.flags=VOICE_EN},
        {.key="dialog/line",.at=200,.bytes=32,.gain=1,.flags=VOICE_RU},
        {.key="dialog/only_ru",.at=300,.bytes=32,.gain=1,.flags=VOICE_RU},
        {.key="weapons/fire",.at=400,.bytes=32,.gain=1,.flags=2}
    };
    records=clips;record_count=4;voice_languages=3;
    assert(bank_voice_languages()==3);
    ui_settings.voice=0;
    for(unsigned i=0;i<32;i++)assert(find_clip("dialog/line")==clips+1);
    voices[1].clip=clips+1;sounds=NULL;
    ui_settings.voice=1;
    for(unsigned i=0;i<32;i++)assert(find_clip("dialog/line")==clips);
    assert(voices[1].clip==clips+1); // An active Russian line keeps its original storage.
    assert(find_clip("dialog/only_ru")==clips+2); // Missing alternate is safe.
    assert(find_clip("weapons/fire")==clips+3); // Legacy/common sounds are independent.
    assert(!find_clip("missing/line"));
    puts("Voice selection, active-clip stability, legacy sound and missing-alternate fallback passed");
    return 0;
}
