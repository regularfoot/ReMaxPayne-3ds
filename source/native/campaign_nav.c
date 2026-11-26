#include "campaign_nav.h"
#include "campaign_file.h"
#include "director.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
typedef struct {Point pos;uint16_t room,count;uint32_t first;} Node;
static Node *nodes;
static uint16_t *edges,*queue,*parent;
static unsigned count;
static int exit_owner[DIRECTOR_EXITS];
static NavPassage passage_guard;
void campaign_nav_guard(NavPassage guard){passage_guard=guard;}
void campaign_nav_reset(NavAgent *agent){*agent=(NavAgent){.next=-1};}
void campaign_nav_event(NavAgent *agent,const char *op){
    if(!strcmp(op,"c_teleport") || !strcmp(op,"c_goto") || !strcmp(op,"c_gotoplayer") ||
       !strcmp(op,"c_gotoandshoot") || !strcmp(op,"c_patrol"))campaign_nav_reset(agent);
}
void campaign_nav_close(void) {passage_guard=NULL;free(nodes);free(edges);free(queue);free(parent);nodes=NULL;edges=queue=parent=NULL;count=0;}
bool campaign_nav_open(const char *stage) {
    char name[64];snprintf(name,sizeof name,"%s.nav",stage);
    FILE *f=campaign_file(name);if(!f)return false;
    struct {char tag[4];uint32_t nodes,edges;} h;
    bool ok=fread(&h,sizeof h,1,f)==1 && !memcmp(h.tag,"MPN1",4) && h.nodes<65535 && h.edges<500000;
    if(ok && h.nodes) {
        nodes=malloc(h.nodes*sizeof(Node));edges=malloc((h.edges?h.edges:1)*2);
        queue=malloc(h.nodes*2);parent=malloc(h.nodes*2);
        ok=nodes && edges && queue && parent && fread(nodes,sizeof(Node),h.nodes,f)==h.nodes && fread(edges,2,h.edges,f)==h.edges;
        for(unsigned i=0;ok && i<h.nodes;i++) ok=nodes[i].first<=h.edges && nodes[i].count<=h.edges-nodes[i].first;
        for(unsigned i=0;ok && i<h.edges;i++) ok=edges[i]<h.nodes;
        count=h.nodes;
    }
    if(ok)ok=fgetc(f)==EOF;
    fclose(f);if(!ok){campaign_nav_close();return false;}
    for(unsigned i=0;i<DIRECTOR_EXITS;i++)exit_owner[i]=-1;
    for(unsigned i=0;i<director.count;i++) if(director.def[i].exit!=DIRECTOR_NONE)exit_owner[director.def[i].exit]=i;
    return true;
}
static int closest(Point p,unsigned room) {
    int best=-1;float cost=FLT_MAX;
    for(unsigned i=0;i<count;i++) {
        const Node *n=nodes+i;if(n->room!=room)continue;
        float x=n->pos.x-p.x,y=n->pos.y-p.y-1.f,z=n->pos.z-p.z,d=x*x+y*y+z*z;
        if(d<cost){cost=d;best=i;}
    }
    return best;
}
static bool connected(unsigned a,unsigned b) {
    for(unsigned i=0;i<DIRECTOR_EXITS;i++) if(exit_owner[i]>=0 && director.exits[i]) {
        const EntityDef *e=director.def+exit_owner[i];if(e->pair>=DIRECTOR_EXITS) continue;
        int other=exit_owner[e->pair];
        if(e->room==a && other>=0 && director.def[other].room==b)return true;
    }
    return false;
}
static int route(int start,int goal,bool allow_closed) {
    if(start<0 || goal<0 || start==goal)return -1;
    memset(parent,0xff,count*2);queue[0]=start;parent[start]=start;
    unsigned head=0,tail=1;
    while(head<tail && parent[goal]==UINT16_MAX) {
        unsigned i=queue[head++];Node n=nodes[i];
        for(unsigned k=0;k<n.count;k++) {
            unsigned j=edges[n.first+k];
            if(parent[j]!=UINT16_MAX || (!allow_closed && n.room!=nodes[j].room && !connected(n.room,nodes[j].room)))continue;
            parent[j]=i;queue[tail++]=j;
        }
    }
    if(parent[goal]==UINT16_MAX && allow_closed) {
        int nearest=-1;float cost=FLT_MAX;
        for(unsigned k=0;k<tail;k++) {
            int id=queue[k];if(nodes[id].room!=nodes[goal].room)continue;
            Point a=nodes[id].pos,b=nodes[goal].pos;
            float dx=a.x-b.x,dy=a.y-b.y,dz=a.z-b.z,d=dx*dx+dy*dy+dz*dz;
            if(d<cost){nearest=id;cost=d;}
        }
        goal=nearest;
    }
    if(goal<0 || goal==start || parent[goal]==UINT16_MAX)return -1;
    while(parent[goal]!=start)goal=parent[goal];
    return goal;
}
bool campaign_nav_step(NavAgent *agent,Point *from,uint16_t *room,Point target,unsigned target_room,float dt,unsigned visible,float *yaw,bool crouched) {
    agent->portal_travel=0;
    if(!count)return false;
    bool following=agent->following && agent->goal_room==target_room && agent->next>=0;
    int reached=-1;
    if(following){
        const Node *n=nodes+agent->next;
        if(n->room==*room && hypotf(n->pos.x-from->x,n->pos.z-from->z)<=.12f &&
           fabsf(n->pos.y-1.f-from->y)<=.6f){
            reached=agent->next;following=false;
        }
    }
    if(!following){
        int start=reached>=0?reached:closest(*from,*room),goal=closest(target,target_room);
        int next=route(start,goal,false);
        if(next<0)next=route(start,goal,true);
        if(next>=0 && nodes[next].room!=*room && hypotf(nodes[start].pos.x-from->x,nodes[start].pos.z-from->z)>.5f)next=start;
        agent->next=next;agent->goal_room=target_room;agent->following=next>=0;agent->remaining=.35f;
    }
    int next=agent->next;
    if(next>=0){
        Node n=nodes[next];target=n.pos;target.y-=1.f;
        if(n.room!=*room){
            if(!connected(*room,n.room)){agent->following=false;return false;}
            if(director.controls && passage_guard){
                if(passage_guard(room,from,n.room,target,crouched,2.f*dt,yaw,&agent->portal_travel))return true;
                agent->following=false;return false;
            }
            *room=n.room;*from=target;return true;
        }
    } else if(*room!=target_room)return false;
    float dx=target.x-from->x,dz=target.z-from->z,d=hypotf(dx,dz);
    if(d<.12f){agent->remaining=0;return false;}
    *yaw=atan2f(dx,dz);float step=fminf(d,dt*2.f)/d;
    /* Scripted blocking follows the authored AI graph. Its walkable edges also
       span gaps in the render mesh, for example the helicopter pad approach. */
    if(*room==visible && director.controls){
        Player p={.position=*from,.crouched=crouched};room_move(&p,dx*step,dz*step);
        float moved=hypotf(p.position.x-from->x,p.position.z-from->z);*from=p.position;
        if(moved>.0001f){agent->remaining=.35f;return true;}
        agent->remaining-=dt;if(agent->remaining<=0)agent->following=false;
        return false;
    }
    from->x+=dx*step;from->z+=dz*step;from->y+=(target.y-from->y)*step;return true;
}
