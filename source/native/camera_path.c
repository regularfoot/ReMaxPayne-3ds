#include "camera_path.h"
#include "campaign_file.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
typedef struct {char name[32];uint32_t first,count;float seconds,fps;} Track;
typedef struct {Point rows[4];} Frame;
static Track tracks[64];
static Frame *frames;
static unsigned count;
void camera_bank_close(void){free(frames);frames=NULL;count=0;}
bool camera_bank_open(void) {
    camera_bank_close();FILE *file=campaign_file("cameras.pak");if(!file)return false;
    struct {char tag[4];uint32_t count,frames;} h;
    bool ok=fread(&h,sizeof h,1,file)==1 && !memcmp(h.tag,"MPCV",4) && h.count<=64 && h.frames && h.frames<32768;
    if(ok){frames=malloc(h.frames*sizeof(*frames));ok=frames && fread(tracks,sizeof(Track),h.count,file)==h.count &&
        fread(frames,sizeof(*frames),h.frames,file)==h.frames && fgetc(file)==EOF;}
    for(unsigned i=0;ok && i<h.count;i++){
        Track t=tracks[i];
        ok=memchr(t.name,0,sizeof t.name) && t.count>=2 && t.first<=h.frames && t.count<=h.frames-t.first && t.seconds>0 && t.fps>0;
    }
    fclose(file);if(ok)count=h.count;else camera_bank_close();return ok;
}
int camera_path_id(const char *name){for(unsigned i=0;i<count;i++)if(!strcmp(name,tracks[i].name))return i;return -1;}
float camera_path_duration(unsigned i){return i<count?tracks[i].seconds:0;}
float camera_path_rate(unsigned i){return i<count?tracks[i].fps:0;}
static Point basis(const Camera *c,Point p) {
    return (Point){p.x*c->right.x+p.y*c->up.x+p.z*c->forward.x,p.x*c->right.y+p.y*c->up.y+p.z*c->forward.y,
        p.x*c->right.z+p.y*c->up.z+p.z*c->forward.z};
}
static Point unit(Point p) {
    float d=fmaxf(.00001f,hypotf(hypotf(p.x,p.y),p.z));return (Point){p.x/d,p.y/d,p.z/d};
}
static Point cross(Point a,Point b){return (Point){a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
void camera_path_view(Camera *c,unsigned id,float time,const Player *parent) {
    if(id>=count)return;
    Track t=tracks[id];float x=fminf(t.count-1,fmaxf(0,time/t.seconds*(t.count-1)));
    unsigned i=fminf(floorf(x),t.count-2);float f=x-i;
    Camera base=*c;
    if(parent) {
        float sn=sinf(parent->yaw),cs=cosf(parent->yaw);
        base=(Camera){.eye=parent->position,.right={cs,0,-sn},.up={0,1,0},.forward={sn,0,cs}};
    }
    Point dirs[4];const Frame *a=frames+t.first+i;
    for(unsigned j=0;j<4;j++){
        Point v=a[0].rows[j],w=a[1].rows[j];
        dirs[j]=basis(&base,(Point){v.x+(w.x-v.x)*f,v.y+(w.y-v.y)*f,v.z+(w.z-v.z)*f});
    }
    c->eye=(Point){base.eye.x+dirs[3].x,base.eye.y+dirs[3].y,base.eye.z+dirs[3].z};
    c->forward=unit(dirs[2]);c->right=unit(cross(dirs[1],c->forward));c->up=unit(cross(c->forward,c->right));
    if(parent)c->distance=hypotf(hypotf(dirs[3].x,dirs[3].y),dirs[3].z);
}
