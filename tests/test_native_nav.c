/* Test the real AI node crossing with physical-passage denial and recovery. */
#include "../source/native/campaign_nav.c"
#include <assert.h>
Director director;
FILE *campaign_file(const char *name){(void)name;return NULL;}
void room_move(Player *p,float x,float z){p->position.x+=x;p->position.z+=z;}
static unsigned calls;
static bool open_door;
static bool passage(uint16_t *room,Point *from,unsigned destination,Point target,bool crouched,
                    float stride,float *yaw,float *travel){
    calls++;
    assert(*room==0 && destination==1 && from->y==0 && target.x==1 && target.y==0 && crouched);
    if(!open_door)return false;
    *travel=stride;*yaw=1.5707963f;from->x+=stride;
    if(from->x>=.25f)*room=destination;
    return true;
}
int main(void){
    EntityDef exits[2]={{.room=0,.exit=0,.pair=1},{.room=1,.exit=1,.pair=0}};
    director.def=exits;director.count=2;director.controls=true;
    director.exits[0]=director.exits[1]=true;
    for(unsigned i=0;i<DIRECTOR_EXITS;i++)exit_owner[i]=-1;
    exit_owner[0]=0;exit_owner[1]=1;
    count=2;nodes=calloc(2,sizeof *nodes);edges=calloc(2,sizeof *edges);
    queue=calloc(2,sizeof *queue);parent=calloc(2,sizeof *parent);
    assert(nodes && edges && queue && parent);
    nodes[0]=(Node){.pos={0,1,0},.room=0,.count=1,.first=0};
    nodes[1]=(Node){.pos={1,1,0},.room=1,.count=1,.first=1};edges[0]=1;edges[1]=0;
    campaign_nav_guard(passage);
    NavAgent a={0};Point from={0},target={1,0,0};uint16_t room=0;float yaw=0;
    assert(!campaign_nav_step(&a,&from,&room,target,1,.05f,0,&yaw,true));
    assert(calls==1 && room==0 && from.x==0 && !a.following);
    open_door=true;
    assert(campaign_nav_step(&a,&from,&room,target,1,.05f,0,&yaw,true));
    assert(calls==2 && room==0 && fabsf(from.x-.1f)<.0001f);
    assert(campaign_nav_step(&a,&from,&room,target,1,.05f,0,&yaw,true));
    assert(campaign_nav_step(&a,&from,&room,target,1,.05f,0,&yaw,true));
    assert(calls==4 && room==1 && fabsf(from.x-.3f)<.0001f && a.portal_travel>0);
    assert(campaign_nav_step(&a,&from,&room,target,1,.05f,0,&yaw,true));
    assert(fabsf(from.x-.4f)<.0001f && a.portal_travel==0);
    from=(Point){0};room=0;a=(NavAgent){0};director.controls=false;open_door=false;
    assert(campaign_nav_step(&a,&from,&room,target,1,.05f,0,&yaw,true));
    assert(calls==4 && room==1); // Authored cinematic blocking keeps its graph.
    from=(Point){0};room=0;a=(NavAgent){0};director.controls=true;
    director.exits[0]=director.exits[1]=false;open_door=true;
    assert(!campaign_nav_step(&a,&from,&room,target,1,.05f,0,&yaw,true));
    assert(calls==4 && room==0 && from.x==0);
    campaign_nav_close();
    // An enemy can be teleported/newly ordered while its previous target was
    // in this same room. The old next node must not send it back the other way.
    count=4;nodes=calloc(count,sizeof *nodes);edges=calloc(6,sizeof *edges);
    queue=calloc(count,sizeof *queue);parent=calloc(count,sizeof *parent);
    assert(nodes && edges && queue && parent);
    const uint16_t line[]={1,0,2,1,3,2};memcpy(edges,line,sizeof line);
    nodes[0]=(Node){.pos={0,1,0},.room=0,.count=1,.first=0};
    nodes[1]=(Node){.pos={1,1,0},.room=0,.count=2,.first=1};
    nodes[2]=(Node){.pos={2,1,0},.room=0,.count=2,.first=3};
    nodes[3]=(Node){.pos={3,1,0},.room=0,.count=1,.first=5};
    const char *commands[]={"c_teleport","c_goto","c_gotoplayer","c_gotoandshoot","c_patrol"};
    for(unsigned i=0;i<sizeof commands/sizeof *commands;i++){
        a=(NavAgent){.next=1,.following=true,.goal_room=0};room=0;from=(Point){2,0,0};target=(Point){3,0,0};
        campaign_nav_event(&a,commands[i]);
        assert(campaign_nav_step(&a,&from,&room,target,0,.05f,0,&yaw,false));
        assert(from.x>2 && yaw>0); // Towards the new target, not the cached node behind him.
    }
    campaign_nav_close();
    puts("New script orders and teleports discard old routes even when the destination room is unchanged.");
    puts("AI waits for a physical doorway, resumes after opening and preserves scripted blocking.");
    return 0;
}
