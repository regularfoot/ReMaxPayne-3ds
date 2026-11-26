// Inputs and outputs captured from retail 110A60/117100/10F960, before skinning.
#include "bone_pose.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "pose_bank.h"
#include "campaign_file.h"
FILE *campaign_file(const char *name){return fopen(name,"rb");}
void probe_mix_local(const Matrix *a,const Matrix *b,Matrix *out,float t,unsigned count){
    for(unsigned i=0;i<count;i++){out[i]=bone_mix_local(a+i,b+i,t);bone_normalize(out+i,2);}
}
static void read_exact(FILE *f,void *out,size_t n){if(fread(out,1,n,f)!=n)abort();}
int main(int argc,char **argv){
    if(argc!=2)return 2;FILE *f=fopen(argv[1],"rb");if(!f)return 2;
    struct {char tag[4];uint32_t cases,bones;} h;read_exact(f,&h,sizeof h);
    if(memcmp(h.tag,"XBP2",4) || h.cases!=26 || h.bones!=28)return 2;
    if(!pose_bank_open())return 2;
    unsigned skin=pose_bank_skin("max_payne"),clips[2]={pose_motion_action(skin,"shootberetta"),pose_motion_action(skin,"shootlowberetta")};
    float worst=0,sampling_error=0;unsigned checked=0;
    for(unsigned row=0;row<h.cases;row++){
        float args[4];Matrix a[28],b[28],expected[28];read_exact(f,args,sizeof args);
        read_exact(f,a,sizeof a);read_exact(f,b,sizeof b);read_exact(f,expected,sizeof expected);
        for(unsigned channel=0;channel<2;channel++){
            unsigned first,last;float mix;Matrix p[29],q[29];
            if(!pose_bank_frames(clips[channel],args[2+channel]/pose_bank_rate(clips[channel]),29,&first,&last,&mix) ||
               !pose_bank_read_local(clips[channel],first,29,p) || !pose_bank_read_local(clips[channel],last,29,q))return 2;
            for(unsigned bone=0;bone<28;bone++){
                Matrix actual=bone_mix_local(p+bone,q+bone,mix);const Matrix *wanted=channel?b+bone:a+bone;
                const uint8_t *mode=pose_bank_normalization(clips[channel]);const float (*scale)[3]=pose_bank_scaling(clips[channel]);
                if(!bone_temporal_finish(&actual,mode[bone],scale?scale[bone]:NULL))return 1;
                for(unsigned r=0;r<3;r++)for(unsigned c=0;c<4;c++){
                    float error=fabsf(actual.row[r][c]-wanted->row[r][c]);sampling_error=fmaxf(sampling_error,error);
                    if(!isfinite(error) || error>.000002f){fprintf(stderr,"Xbox temporal sample differs at case %u channel %u bone %u: %g\n",row,channel,bone,error);return 1;}
                }
            }
        }
        for(unsigned bone=0;bone<28;bone++){
            // The captured first channel is weighted by args[0].
            Matrix actual=bone_mix_local(b+bone,a+bone,args[0]);
            if(!bone_normalize(&actual,2))return 1;
            for(unsigned r=0;r<3;r++)for(unsigned c=0;c<4;c++){
                float error=fabsf(actual.row[r][c]-expected[bone].row[r][c]);worst=fmaxf(worst,error);
                if(!isfinite(error) || error>.000002f){fprintf(stderr,"Xbox bone %u case %u differs: %g\n",bone,row,error);return 1;}
            }
            checked++;
        }
    }
    if(fgetc(f)!=EOF)return 2;fclose(f);pose_bank_close();
    printf("Retail Xbox temporal sampling: %u source transforms, max element error %.9g\n",checked*2,sampling_error);
    printf("Retail Xbox bone oracle: %u local transforms / %u blends, max element error %.9g\n",checked,h.cases,worst);return 0;
}
