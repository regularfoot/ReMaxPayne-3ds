#include "portal.h"
#include <math.h>
typedef struct {float u,v;} UV;
static unsigned plane(Point n){
    return fabsf(n.x)>fabsf(n.y) && fabsf(n.x)>fabsf(n.z)?0:fabsf(n.y)>fabsf(n.z)?1:2;
}
static UV project(Point p,unsigned axis){
    return axis==0?(UV){p.y,p.z}:axis==1?(UV){p.z,p.x}:(UV){p.x,p.y};
}
static float area(UV a,UV b,UV c){return (b.u-a.u)*(c.v-a.v)-(b.v-a.v)*(c.u-a.u);}
static bool inside(UV a,UV b,UV c,UV p,float winding,float epsilon){
    return winding*area(a,b,p)>=-epsilon && winding*area(b,c,p)>=-epsilon && winding*area(c,a,p)>=-epsilon;
}
unsigned portal_triangles(const Point *vertices,unsigned count,Point normal,uint8_t *out){
    if(count<3 || count>PORTAL_VERTICES)return 0;
    unsigned axis=plane(normal),n=count,written=0;
    UV v[PORTAL_VERTICES];uint8_t ring[PORTAL_VERTICES];float total=0;
    for(unsigned i=0;i<n;i++){v[i]=project(vertices[i],axis);ring[i]=i;}
    for(unsigned i=1;i+1<n;i++)total+=area(v[0],v[i],v[i+1]);
    float winding=total<0?-1:1;
    while(n>2){
        bool found=false;
        for(unsigned i=0;i<n;i++){
            unsigned a=ring[(i+n-1)%n],b=ring[i],c=ring[(i+1)%n];
            float turn=winding*area(v[a],v[b],v[c]);
            if(turn<-.000002f)continue;
            if(turn>.000002f){
                bool occupied=false;
                for(unsigned j=0;j<n;j++){
                    unsigned k=ring[j];
                    if(k!=a && k!=b && k!=c && inside(v[a],v[b],v[c],v[k],winding,.000001f)){occupied=true;break;}
                }
                if(occupied)continue;
                out[written++]=a;out[written++]=b;out[written++]=c;
            }
            for(unsigned j=i+1;j<n;j++)ring[j-1]=ring[j];
            n--;found=true;break;
        }
        if(!found)return 0;
    }
    return written;
}
bool portal_contains(const Point *vertices,const uint8_t *indices,unsigned count,Point normal,Point pos){
    unsigned axis=plane(normal);UV p=project(pos,axis);
    for(unsigned i=0;i<count;i+=3){
        UV a=project(vertices[indices[i]],axis),b=project(vertices[indices[i+1]],axis),c=project(vertices[indices[i+2]],axis);
        if(inside(a,b,c,p,area(a,b,c)<0?-1:1,.00001f))return true;
    }
    return false;
}

bool portal_departure(const Point *vertices,const uint8_t *indices,unsigned count,Point normal,
                      Point previous,Point position,Point intent,float *fraction){
    Point origin=vertices[0];
    float before=(previous.x-origin.x)*normal.x+(previous.y-origin.y)*normal.y+(previous.z-origin.z)*normal.z;
    float after=(position.x-origin.x)*normal.x+(position.y-origin.y)*normal.y+(position.z-origin.z)*normal.z;
    float toward=intent.x*normal.x+intent.y*normal.y+intent.z*normal.z;
    float travel=after-before;
    if(travel>=-.00001f && toward>=-.00001f)return false;
    if(before<-.45f || (before<0 && after<-.45f))return false;
    bool seam=after>.025f && toward<-.00001f && -travel<-.25f*toward;
    if(after>(seam?.32f:.025f))return false;
    // The endpoint can have left the polygon again during a diagonal/long step.
    // Test where the segment crossed the actual opening, not its final projection.
    float t=travel<-.00001f?fminf(1,fmaxf(0,-before/travel)):1;
    Point at={previous.x+t*(position.x-previous.x),previous.y+t*(position.y-previous.y),
              previous.z+t*(position.z-previous.z)};
    if(!portal_contains(vertices,indices,count,normal,at))return false;
    *fraction=t;return true;
}

bool portal_nearby(const Point *vertices,const uint8_t *indices,unsigned count,Point n,Point center){
    if(count<3)return false;
    Point o=vertices[0];float d=(center.x-o.x)*n.x+(center.y-o.y)*n.y+(center.z-o.z)*n.z;
    return fabsf(d)<=3.5f && portal_contains(vertices,indices,count,n,
        (Point){center.x-d*n.x,center.y-d*n.y,center.z-d*n.z});
}
