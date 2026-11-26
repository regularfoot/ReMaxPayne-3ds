#include "room.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

static int16_t grid[4];
static uint8_t *walkable;
static int16_t *next,*queue;
static int destination=-1;
void nav_close(void) {
    free(walkable);free(next);free(queue);
    walkable=NULL;next=NULL;queue=NULL;destination=-1;
}
bool nav_open(void) {
    FILE *f=fopen("romfs:/street.nav","rb");
    if (!f) return false;
    bool ok=fread(grid,sizeof grid,1,f)==1 && grid[2]>1 && grid[3]>1 && grid[2]*grid[3]<30000;
    if (ok) {
        unsigned n=grid[2]*grid[3];
        walkable=malloc(n);next=malloc(n*sizeof(*next));queue=malloc(n*sizeof(*queue));
        ok=walkable && next && queue && fread(walkable,1,n,f)==n;
    }
    fclose(f);
    if (!ok) nav_close();
    return ok;
}
static int closest(Point p,bool connected) {
    int x=lroundf(p.x)-grid[0],z=lroundf(p.z)-grid[1],best=-1;
    float d=1e9f;
    for (int j=z-3;j<=z+3;j++) for (int i=x-3;i<=x+3;i++) {
        if (i<0 || j<0 || i>=grid[2] || j>=grid[3]) continue;
        int at=j*grid[2]+i;
        if (!walkable[at] || (connected && next[at]<0)) continue;
        float dx=i+grid[0]-p.x,dz=j+grid[1]-p.z,dd=dx*dx+dz*dz;
        if (dd<d){best=at;d=dd;}
    }
    return best;
}
Point room_pursue(Point from,Point to,bool visible) {
    int goal=(!visible && walkable)?closest(to,false):-1;
    if (goal>=0 && goal!=destination) {
        int w=grid[2],n=w*grid[3];
        memset(next,0xff,n*sizeof(*next));destination=goal;next[goal]=goal;
        unsigned head=0,tail=0;queue[tail++]=goal;
        while (head<tail) {
            int a=queue[head++];
            int neighbours[4]={a-w,a+w,a%w?a-1:-1,(a+1)%w?a+1:-1};
            for (int i=0;i<4;i++) {
                int b=neighbours[i];
                if (b<0 || b>=n || !walkable[b] || next[b]>=0) continue;
                next[b]=a;queue[tail++]=b;
            }
        }
    }
    if (goal>=0) {
        int cell=closest(from,true);
        if (cell>=0 && cell!=goal) {
            int at=next[cell];
            to.x=grid[0]+at%grid[2];to.z=grid[1]+at/grid[2];
        }
    }
    float x=to.x-from.x,z=to.z-from.z,len=hypotf(x,z);
    return len>.01f?(Point){x/len,0,z/len}:(Point){0};
}
