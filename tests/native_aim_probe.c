// Numerical oracle captured at 18AD20/18B870 in the running retail Xbox XBE.
// The expected values were produced by the original controller, independently
// of this implementation. See work/port-audit/aim-controller-20261123/report.md.
#include "aim_pose.h"
#include "pose_bank.h"
#include "campaign_file.h"
FILE *campaign_file(const char *name){return fopen(name,"rb");}
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void read_exact(FILE *f,void *out,size_t bytes){if(fread(out,1,bytes,f)!=bytes)abort();}
int main(int argc,char **argv){
    if(argc!=2)return 2;
    FILE *f=fopen(argv[1],"rb");if(!f)return 2;
    struct {char tag[4];uint32_t weapons,cases,yaw;} h;read_exact(f,&h,sizeof h);
    if(memcmp(h.tag,"XAM1",4) || h.weapons!=17 || h.cases!=306 || h.yaw!=32)return 2;
    AimTable tables[2][17];read_exact(f,tables,sizeof tables);
    if(!pose_bank_open())return 2;
    const char *weapons[]={"empty","leadpipe","baseballbat","beretta","berettadual","deserteagle","sawedshotgun","pumpshotgun","ingram","ingramdual","mp5","jackhammer","molotov","grenade","m79","sniper","painkiller"};
    unsigned skin=pose_bank_skin("max_payne");
    Matrix neutral[64],extreme[64],scratch[64],inverse[64];short parent[64];
    FILE *tree=campaign_file("bones.tree");uint32_t tree_header[2];
    if(!tree || fread(tree_header,4,2,tree)!=2 || tree_header[1]!=29 || fread(parent,2,29,tree)!=29)return 2;
    fclose(tree);for(unsigned i=0;i<29;i++)inverse[i]=bone_inverse_frame(pose_bank_rest()+i);
    float angle_error=0,position_error=0;
    for(unsigned weapon=0;weapon<17;weapon++)for(unsigned high=0;high<2;high++){
        char name[64];snprintf(name,sizeof name,"shoot%s",weapons[weapon]);unsigned a=pose_motion_action(skin,name);
        snprintf(name,sizeof name,"shoot%s%s",high?"high":"low",weapons[weapon]);unsigned b=pose_motion_action(skin,name);
        if(!a || !b || !pose_bank_read_local(a,0,29,neutral) || !pose_bank_read_local(b,0,29,extreme))return 2;
        AimTable actual;if(!aim_calibrate(neutral,extreme,scratch,inverse,parent,29,28,&actual))return 1;
        const AimTable *expected=&tables[!high][weapon];
        for(unsigned frame=0;frame<16;frame++){
            float error=fabsf(actual.angle[frame]-expected->angle[frame]);angle_error=fmaxf(angle_error,error);
            if(!isfinite(error) || error>.0001f){fprintf(stderr,"Xbox calibration angle differs for %s %u: %g\n",weapons[weapon],frame,error);return 1;}
            for(unsigned i=0;i<3;i++){
                float error=fabsf(actual.position[frame][i]-expected->position[frame][i]);position_error=fmaxf(position_error,error);
                if(!isfinite(error) || error>.00002f){fprintf(stderr,"Xbox calibration position differs for %s %u: %g\n",weapons[weapon],frame,error);return 1;}
            }
        }
    }
    pose_bank_close();
    printf("Complete Xbox calibration: 34 tables / 544 samples, max elevation error %.8g rad, position error %.8g m\n",angle_error,position_error);
    float worst=0,worst_yaw=0;
    for(unsigned i=0;i<h.cases;i++){
        struct {uint32_t weapon,high;float target[3],weight;} r;read_exact(f,&r,sizeof r);
        if(r.weapon>=17 || r.high>2)return 2;
        float weight=0;
        if(r.high!=2){
            int high=aim_high(&tables[1][r.weapon],r.target);
            if(high!=(int)r.high){fprintf(stderr,"Aim direction differs from Xbox in case %u\n",i);return 1;}
            weight=aim_extreme_weight(&tables[!high][r.weapon],r.target,high);
        }
        float error=fabsf(weight-r.weight);worst=fmaxf(worst,error);
        if(!isfinite(weight) || error>.00002f){fprintf(stderr,"Xbox aim case %u: %g versus %g\n",i,weight,r.weight);return 1;}
    }
    for(unsigned i=0;i<h.yaw;i++){
        struct {float gun[3],pivot[3],target[3],alpha,axis[3],expected[9];} r;read_exact(f,&r,sizeof r);
        float angle=aim_torso_yaw(r.gun,r.pivot,r.target,r.alpha),s=sinf(angle),c=cosf(angle);
        float length=sqrtf(r.axis[0]*r.axis[0]+r.axis[1]*r.axis[1]+r.axis[2]*r.axis[2]);
        for(unsigned k=0;k<3;k++)r.axis[k]/=length;
        // Compare the full original bone-space rotation, including its axis.
        for(unsigned a=0;a<3;a++)for(unsigned b=0;b<3;b++){
            unsigned k=(a+1)%3;float cross=0;
            if(b==k)cross=r.axis[(a+2)%3];else if(b==(a+2)%3)cross=-r.axis[k];
            float actual=(a==b?c:0)+(1-c)*r.axis[a]*r.axis[b]+s*cross;
            float error=fabsf(actual-r.expected[3*a+b]);worst_yaw=fmaxf(worst_yaw,error);
            if(!isfinite(actual) || error>.00002f){fprintf(stderr,"Xbox Torso case %u differs: %g\n",i,error);return 1;}
        }
    }
    if(fgetc(f)!=EOF)return 2;fclose(f);
    Matrix palette[5];for(unsigned i=0;i<5;i++)palette[i]=(Matrix){.row={{1,0,0,i*.1f},{0,1,0,i*.2f},{0,0,1,i*.3f}}};
    Matrix before[5];memcpy(before,palette,sizeof before);
    const short small_parent[5]={-1,0,1,2,0};const float pivot[3]={.1f,.2f,.3f};
    aim_turn_torso(palette,5,1,small_parent,pivot,.63f);
    if(memcmp(palette,before,sizeof *palette) || memcmp(palette+4,before+4,sizeof *palette))return 1;
    for(unsigned k=0;k<3;k++)if(fabsf(palette[1].row[k][3]-pivot[k])>.00001f)return 1;
    printf("Xbox aim oracle: %u weapon/target cases, %u Torso matrices; max weight error %.8g, matrix error %.8g; pelvis/legs unchanged\n",h.cases,h.yaw,worst,worst_yaw);
    return 0;
}
