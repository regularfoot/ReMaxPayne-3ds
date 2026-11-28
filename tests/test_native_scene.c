#include "../source/native/scene.h"
#include "../source/native/portal.h"
#include "../source/native/touch.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void test_pause_camera(void){
    assert(scene_open(NULL,0));
    for(unsigned crouch=0;crouch<2;crouch++){
        const Player player={.position={3,2,-4},.yaw=.6f,.pitch=.4f,.crouched=crouch};
        Camera entry={0};scene_camera(&entry,&player,0);Camera saved=entry;
        PauseCamera orbit;scene_pause_begin(&orbit,&entry,&player);
        Point start=entry.eye;
        for(unsigned frame=0;frame<1800;frame++){
            scene_pause_step(&orbit,1.f/60,scene_trace);
            const Camera *c=&orbit.view;
            Point delta={orbit.pivot.x-c->eye.x,orbit.pivot.y-c->eye.y,orbit.pivot.z-c->eye.z};
            float length=hypotf(delta.x,hypotf(delta.y,delta.z));
            assert(length>1.85f && length<2.9f);
            assert((delta.x*c->forward.x+delta.y*c->forward.y+delta.z*c->forward.z)/length>.999f);
            assert(fabsf(c->forward.x*c->right.x+c->forward.y*c->right.y+c->forward.z*c->right.z)<.0001f);
        }
        assert(hypotf(orbit.view.eye.x-start.x,orbit.view.eye.z-start.z)>.5f);
        assert(!memcmp(&entry,&saved,sizeof entry));
    }
    scene_close();
    // A full-height side wall stops the orbit; the camera must reverse without
    // crossing the wall, jumping to the opposite side or advancing the player.
    const Triangle wall[]={{{1,-5,-10},{1,8,-10},{1,8,10},0},{{1,-5,-10},{1,8,10},{1,-5,10},0}};
    assert(scene_open(wall,2));Player player={0};Camera entry={0};scene_camera(&entry,&player,0);
    PauseCamera orbit;scene_pause_begin(&orbit,&entry,&player);bool reversed=false;
    for(unsigned frame=0;frame<2400;frame++){
        Point previous=orbit.view.eye;float before=orbit.direction;
        scene_pause_step(&orbit,1.f/60,scene_trace);
        if(before!=orbit.direction)reversed=true;
        assert(orbit.view.eye.x<.91f);
        assert(hypotf(orbit.view.eye.x-previous.x,orbit.view.eye.z-previous.z)<.03f);
        assert(!player.position.x && !player.position.y && !player.position.z);
    }
    assert(reversed);scene_close();
}
static void test_training_ledge(float height,int crossing){
 Triangle t[]={
 {{-10,0,-5},{10,0,5},{10,0,-5},1},{{-10,0,-5},{-10,0,5},{10,0,5},1},
 {{0,0,-5},{0,height,-5},{0,height,5},0},{{0,0,-5},{0,height,5},{0,0,5},0},
 {{0,height,-5},{2,height,5},{2,height,-5},1},{{0,height,-5},{0,height,5},{2,height,5},1}};
 assert(scene_open(t,6));Player p={.position={-.6f,0,0}};Jump j={0};float apex=0;
 for(unsigned f=0;f<55;f++){
  scene_move_gravity(&p,&j,2.5f/40,0,1.f/40,f==0,20,7.5f);apex=fmaxf(apex,p.position.y);
 }
 printf("height %.2f x %.3f y %.3f apex %.3f\n",height,p.position.x,p.position.y,apex);
 if(crossing)assert(p.position.x>1.0f);else assert(p.position.x<0);
 assert(apex<1.61f);scene_close();
}

static void test_weapon_picker(void){
    unsigned seen=0;
    for(unsigned slot=0;slot<16;slot++){
        TouchRect a=touch_weapon_rect(slot);
        assert(a.x>=0 && a.y>=45 && a.x+a.w<=320 && a.y+a.h<=240);
        for(unsigned j=0;j<slot;j++){
            TouchRect b=touch_weapon_rect(j);
            assert(a.x>=b.x+b.w || b.x>=a.x+a.w || a.y>=b.y+b.h || b.y>=a.y+a.h);
        }
        int gun=touch_weapon_order[slot];
        if(gun>=0){assert(gun<TOUCH_WEAPON_COUNT && !(seen&(1u<<gun)));seen|=1u<<gun;}
    }
    assert(seen==(1u<<TOUCH_WEAPON_COUNT)-1);
    for(unsigned gun=0;gun<TOUCH_WEAPON_COUNT;gun++){
        WeaponPicker picker={0};
        WeaponChoice choice=touch_weapon_update(&picker,true,30,214,true,0x7fff,0);
        assert(choice.consumed && choice.changed && picker.open);
        unsigned slot=0;while(touch_weapon_order[slot]!=(int)gun)slot++;
        TouchRect r=touch_weapon_rect(slot);
        choice=touch_weapon_update(&picker,true,r.x+10,r.y+10,true,0x7fff,0);
        assert(choice.consumed && choice.changed && !picker.open && choice.selected==(int)gun);
    }
    WeaponPicker picker={0};
    touch_weapon_update(&picker,true,30,214,true,1,0);
    TouchRect unavailable=touch_weapon_rect(1);
    WeaponChoice choice=touch_weapon_update(&picker,true,unavailable.x+10,unavailable.y+10,true,1,0);
    assert(picker.open && choice.selected==0 && !choice.changed && choice.consumed);
    choice=touch_weapon_update(&picker,true,318,238,true,1,0);
    assert(choice.consumed && choice.changed && !picker.open);
    touch_weapon_update(&picker,true,30,214,true,1,0);
    choice=touch_weapon_update(&picker,false,0,0,false,1,0);
    assert(!picker.open && choice.changed);
}

static void test_touch(void) {
    for(unsigned vertical=0;vertical<2;vertical++){
        float stick=touch_look_angle(.6f,0,.02f,100,100,vertical);
        float touch=touch_look_angle(0,11,.02f,100,100,vertical);
        assert(fabsf(touch_look_angle(.6f,11,.02f,25,250,vertical)-(.25f*stick+2.5f*touch))<1e-6f);
        assert(fabsf(touch_look_angle(.6f,11,.02f,250,25,vertical)-(2.5f*stick+.25f*touch))<1e-6f);
        for(unsigned setting=0;setting<=4;setting++){
            unsigned percent=50+25*setting;
            assert(fabsf(touch_look_angle(.6f,11,.02f,percent,percent,vertical)-
                         (stick+touch)*(.5f+.25f*setting))<1e-6f);
        }
    }
    assert(cstick_axis(24,0)==0 && cstick_axis(-24,1)==0);
    assert(cstick_axis(25,1)>0 && cstick_axis(-25,1)==-cstick_axis(25,1));
    assert(cstick_axis(44,4)==0 && cstick_axis(150,4)==1 && cstick_axis(-156,1)==-1);
    TouchInput state={0};
    unsigned mask=TOUCH_SHORTCUTS|TOUCH_BIT(TOUCH_CROUCH)|TOUCH_BIT(TOUCH_SLOW)|TOUCH_BIT(TOUCH_JOURNAL);
    for(int y=0;y<240;y++){
        for(int x=160;x<320;x++){
            assert(touch_update(&state,true,x,y,mask).pressed==TOUCH_LOOK);
            TouchAction drag=touch_update(&state,true,20,205,mask);
            assert(drag.pressed==TOUCH_NONE && drag.dx==20-x && drag.dy==y-205);
            touch_update(&state,false,0,0,mask);
        }
    }
    for(TouchZone zone=TOUCH_WEAPON;zone<=TOUCH_AIM;zone++){
        TouchRect r=touch_rects[zone];
        TouchAction press=touch_update(&state,true,r.x,r.y,mask);
        assert(press.pressed==zone && press.held==zone);
        TouchAction drag=touch_update(&state,true,319,180,mask);
        assert(!drag.dx && !drag.dy && drag.pressed==TOUCH_NONE && drag.held==zone);
        TouchAction release=touch_update(&state,false,0,0,mask);
        assert(release.held==TOUCH_NONE);
        assert(touch_update(&state,true,r.x,r.y,0).pressed==TOUCH_LOOK);
        touch_update(&state,false,0,0,0);
    }
}

static void test_moving_collision(void) {
    Triangle faces[33];
    uint8_t masks[33];
    for(unsigned i=0;i<33;i++){
        faces[i]=(Triangle){{-1,-1,2},{1,-1,2},{0,2,2},0};
        masks[i]=SCENE_BODY|SCENE_SHOT;
    }
    assert(scene_open_masks(faces,33,masks));
    for(unsigned i=15;i<=16;i++){
        float x=i==15?12:-12;
        faces[i].a.x+=x;faces[i].b.x+=x;faces[i].c.x+=x;
    }
    scene_refit(15,2);
    Hit hit;
    assert(scene_trace((Point){12,0,0},(Point){0,0,1},3,&hit) && hit.face==15);
    assert(scene_trace((Point){-12,0,0},(Point){0,0,1},3,&hit) && hit.face==16);
    masks[15]=SCENE_SHOT;
    assert(!scene_trace((Point){12,0,0},(Point){0,0,1},3,&hit));
    faces[32].a.x+=24;faces[32].b.x+=24;faces[32].c.x+=24;
    scene_refit(32,1);
    assert(scene_trace((Point){24,0,0},(Point){0,0,1},3,&hit) && hit.face==32);
    assert(scene_trace((Point){0,0,0},(Point){0,0,1},3,&hit));
    scene_close();
    assert(scene_open(NULL,0));
    assert(!scene_trace((Point){0,0,0},(Point){0,0,1},3,&hit));
    scene_close();
}
static void test_panel_contact(void) {
    // One swinging cupboard leaf over a static floor.
    Triangle faces[]={
        {{-3,0,-3},{3,0,-3},{0,0,3},1},
        {{0,.40f,-1},{0,1.60f,-1},{0,1.60f,1},0},
        {{0,.40f,-1},{0,1.60f,1},{0,.40f,1},0},
    };
    uint8_t masks[]={SCENE_BODY,SCENE_BODY|SCENE_SHOT,SCENE_BODY|SCENE_SHOT};
    assert(scene_open_masks(faces,3,masks));
    Player below={0};
    Triangle overhead={{-1,1.6f,-1},{1,1.6f,-1},{0,1.6f,1},1};
    assert(isinf(scene_panel_distance(&overhead,&below)));
    assert(scene_overhead_distance(&overhead,&below)==0);
    below.position.y=-.3f;
    assert(isinf(scene_overhead_distance(&overhead,&below)));
    for(unsigned crouch=0;crouch<2;crouch++){
        Player max={.position={-.04f,0,0},.crouched=crouch},npc=max;
        Jump jump={0};
        assert(isinf(scene_panel_distance(faces,&max)));
        assert(fabsf(fminf(scene_panel_distance(faces+1,&max),scene_panel_distance(faces+2,&max))-.04f*.04f)<.00001f);
        assert(scene_wall_overlap(1,2,&max,false));
        masks[1]|=SCENE_PLAYER_OVERLAP;masks[2]|=SCENE_PLAYER_OVERLAP;
        room_move(&npc,.04f,0);
        assert(npc.position.x==-.04f);
        Hit hit;
        assert(scene_trace((Point){-.5f,.8f,0},(Point){1,0,0},1,&hit));
        scene_move_player(&max,&jump,.10f,0,1.f/60,false);
        assert(max.position.x==-.04f);
        scene_move_player(&max,&jump,0,.1f,1.f/60,false);
        assert(fabsf(max.position.z-.1f)<.00001f);
        for(unsigned i=0;i<7;i++)scene_move_player(&max,&jump,-.05f,0,1.f/60,false);
        assert(max.position.x<-.25f && !scene_wall_overlap(1,2,&max,false) && !jump.airborne);
        masks[1]=masks[2]=SCENE_BODY|SCENE_SHOT;
        for(unsigned i=0;i<12;i++)scene_move_player(&max,&jump,.05f,0,1.f/60,false);
        assert(max.position.x<=-.22f);
        max.position=(Point){-.3f,0,0};
        scene_move_player(&max,&jump,.6f,0,1.f/60,false);
        assert(max.position.x==-.3f);
    }
    scene_close();
}
static void test_ajar_door(void){
    for(unsigned crouch=0;crouch<2;crouch++)for(unsigned airborne=0;airborne<2;airborne++){
        Player body={.position={0,0,-.4f},.crouched=crouch};
        for(unsigned open=0;open<2;open++){
            float angle=(open?90.f:15.f)*3.14159265f/180.f;
            Point hinge={-.6f,0,0},edge={-.6f+1.2f*cosf(angle),0,1.2f*sinf(angle)};
            Triangle faces[]={
                {hinge,{hinge.x,2,hinge.z},{edge.x,2,edge.z},0},
                {hinge,{edge.x,2,edge.z},edge,0}
            };
            bool blocked=scene_panel_blocks(faces,&body,(Point){0,0,.5f},airborne) ||
                         scene_panel_blocks(faces+1,&body,(Point){0,0,.5f},airborne);
            assert(blocked==!open);
        }
    }
}
static void test_portal_departure(void){
    const Point opening[]={{-1,-1,0},{1,-1,0},{1,2,0},{-1,2,0}};
    uint8_t indices[6];Point n={0,0,-1};float fraction;
    unsigned count=portal_triangles(opening,4,n,indices);
    assert(count==6);
    // Normal motion, a complete high-speed crossing, and platform carry with no input.
    assert(portal_departure(opening,indices,count,n,(Point){0,0,-.1f},(Point){0,0,.1f},(Point){0,0,.2f},&fraction));
    assert(fabsf(fraction-.5f)<.00001f);
    assert(portal_departure(opening,indices,count,n,(Point){0,0,-.1f},(Point){0,0,1},(Point){0,0,1.1f},&fraction));
    assert(portal_departure(opening,indices,count,n,(Point){0,0,-.1f},(Point){0,0,.1f},(Point){0},&fraction));
    // The segment intersects the opening but finishes outside its lateral projection.
    assert(portal_departure(opening,indices,count,n,(Point){0,0,-.1f},(Point){1.2f,0,.1f},(Point){1.2f,0,.2f},&fraction));
    assert(!portal_departure(opening,indices,count,n,(Point){1.2f,0,-.1f},(Point){0,0,-.05f},(Point){-1.2f,0,.05f},&fraction));
    assert(!portal_departure(opening,indices,count,n,(Point){0,0,.1f},(Point){0,0,-.1f},(Point){0,0,-.2f},&fraction));
    assert(!portal_departure(opening,indices,count,n,(Point){0,0,-.1f},(Point){0,0,-.1f},(Point){0},&fraction));
    // Capsule stopped by the source BSP lip, retaining the existing seam support.
    assert(portal_departure(opening,indices,count,n,(Point){0,0,-.22f},(Point){0,0,-.22f},(Point){0,0,.1f},&fraction));
    // A horizontal opening is crossed by gravity, independently of stick direction.
    Point hole[4];for(unsigned i=0;i<4;i++)hole[i]=(Point){opening[i].x,0,opening[i].y};
    n=(Point){0,1,0};count=portal_triangles(hole,4,n,indices);
    assert(portal_departure(hole,indices,count,n,(Point){0,.1f,0},(Point){0,-.1f,0},(Point){0},&fraction));
}
static void test_stair_portal(void) {
    // The opening has recessed lower corners on either side of the central stair.
    Point opening[]={{-2,3,0},{2,3,0},{2,-1,0},{1,-1,0},{1,0,0},
                     {0,0,0},{-1,0,0},{-1,-1,0},{-2,-1,0}};
    Point normal={0,0,-1};uint8_t indices[90];
    for(unsigned side=0;side<2;side++){
        unsigned n=portal_triangles(opening,9,normal,indices);
        assert(n && portal_contains(opening,indices,n,normal,(Point){0,1,0}));
        assert(portal_contains(opening,indices,n,normal,(Point){1.5f,-.5f,0}));
        assert(!portal_contains(opening,indices,n,normal,(Point){0,-.5f,0}));
        for(unsigned i=0;i<4;i++){Point tmp=opening[i];opening[i]=opening[8-i];opening[8-i]=tmp;}
        normal.z=-normal.z;
    }
    Triangle tread={.a={-1,.24f,0},.b={1,.24f,0},.c={0,.24f,2},.normal_y=1};
    assert(scene_open(&tread,1));
    Player p={.position={0,0,.01f}};Jump jump={.airborne=true,.velocity=-.6f};
    assert(scene_settle_step(&p,&jump) && !jump.airborne && fabsf(p.position.y-.24f)<.001f);
    p.position=(Point){0,-1,1};assert(!scene_settle_step(&p,&jump));
    p.position=(Point){0,.55f,1};assert(scene_settle_step(&p,&jump) && fabsf(p.position.y-.24f)<.001f);
    scene_close();
}
static bool door_jamb(Point start,Point dir,float length,Hit *hit){
    (void)start;
    if(dir.x<.08f || length<.44f)return false;
    hit->distance=.44f;return true;
}
static void test_shoulder_door(void){
    Player p={0};Camera c={0};
    assert(scene_open(NULL,0));
    scene_camera(&c,&p,0);
    assert(c.eye.x>.6f && c.eye.z<-2.24f);
    scene_camera_sweep(&c,&p,1.f/60,door_jamb);
    assert(fabsf(c.eye.x)<.01f && c.eye.z<-2.24f);
    for(unsigned i=0;i<30;i++){
        scene_camera_sweep(&c,&p,1.f/60,door_jamb);
        assert(c.eye.z<-2.24f);
    }
    for(unsigned i=0;i<60;i++)scene_camera(&c,&p,1.f/60);
    assert(c.eye.x>.61f && c.eye.z<-2.24f);
    scene_close();
}
static bool boxed_camera(Point start,Point dir,float length,Hit *hit){
    (void)start;(void)dir;
    if(length<.22f)return false;
    hit->distance=.22f;return true;
}
static void test_death_camera(void){
    const Triangle wall[]={
        {{-5,-2,.38f},{5,-2,.38f},{5,4,.38f},0},
        {{-5,-2,.38f},{5,4,.38f},{-5,4,.38f},0},
    };
    assert(scene_open(wall,2));
    Point center={0,.6f,0};
    Camera track={.eye={0,.6f,-2},.forward={0,0,1},.right={1,0,0},.up={0,1,0}};
    assert(scene_death_camera(&track,center,scene_trace) && track.eye.z==-2 && track.forward.z==1);
    track.eye.z=3;track.forward.z=-1;
    assert(scene_death_camera(&track,center,scene_trace));
    assert(track.distance>=1.55f && track.eye.z<.38f);
    assert(fabsf(track.eye.x+track.forward.x*track.distance)<.001f);
    assert(fabsf(track.eye.y+track.forward.y*track.distance-center.y)<.001f);
    assert(fabsf(track.eye.z+track.forward.z*track.distance)<.001f);
    track.eye=center;
    assert(scene_death_camera(&track,center,scene_trace) && track.distance>=1.55f);
    assert(!scene_death_camera(&track,center,boxed_camera));
    scene_close();
}
static void test_jump_height(void){
    const float gravity[]={10,15,20},impulses[]={7.5f,4.5f,3.f};
    Triangle floor={.a={-4,0,-4},.b={4,0,-4},.c={0,0,4},.normal_y=1};
    assert(scene_open(&floor,1));
    for(unsigned rate=30;rate<=120;rate*=2){
        for(unsigned g=0;g<3;g++)for(unsigned v=0;v<3;v++){
            Player p={0};Jump j={0};float apex=0;
            for(unsigned frame=0;frame<2*rate;frame++){
                scene_move_gravity(&p,&j,0,0,1.f/rate,!frame,gravity[g],impulses[v]);
                apex=fmaxf(apex,p.position.y);
            }
            assert(fabsf(apex-impulses[v]*impulses[v]/(2*gravity[g]))<.003f);
            assert(!j.airborne && fabsf(p.position.y)<.0001f);
        }
    }
    scene_close();
}
static void test_idle_floor_loss(void){
    Triangle floor={.a={-2,0,-2},.b={2,0,-2},.c={0,0,2},.normal_y=1};
    uint8_t mask=SCENE_BODY;
    assert(scene_open_masks(&floor,1,&mask));
    Player p={0};Jump jump={0};
    scene_move_gravity(&p,&jump,0,0,1.f/60,false,20,7.5f);
    assert(!jump.airborne && p.position.y==0);
    mask=0;
    scene_move_gravity(&p,&jump,0,0,1.f/60,false,20,7.5f);
    assert(jump.airborne);
    scene_move_gravity(&p,&jump,0,0,1.f/60,false,20,7.5f);
    assert(p.position.y<0 && jump.velocity<0);
    scene_close();
}
static void test_wall_seam(void){
    const Point a={22.9200134f,15.8102779f,19.7444916f},b={20.2386246f,17.501194f,17.8606625f};
    const Point c={24.2780972f,15.2047977f,17.444664f},d={21.5967083f,16.8957138f,15.5608339f};
    const Triangle wall[]={{a,b,c,0},{b,d,c,0}};
    const Point start={8.78964806f,27.3816242f,13.4923897f},dir={.757037401f,-.613824248f,.223861963f};
    Hit hit;
    assert(scene_open(wall,2));
    assert(scene_trace(start,dir,19.f,&hit));
    assert(fabsf(hit.distance-18.37933f)<.0001f);
    assert(!scene_trace((Point){start.x,start.y+2.f,start.z},dir,19.f,&hit));
    scene_close();
}

int main(void) {
    test_wall_seam();
    test_portal_departure();
    test_shoulder_door();
    test_jump_height();
    test_training_ledge(1.5f,1);test_training_ledge(2.f,0);
    test_death_camera();
    test_panel_contact();
    test_ajar_door();
    test_stair_portal();
    test_moving_collision();
    test_idle_floor_loss();
    test_pause_camera();
    test_touch();
    test_weapon_picker();
    // The nearer wall is last in the input to exercise nearest-hit selection.
    const Triangle walls[]={
        {{-4,-2,8},{4,-2,8},{0,6,8},0},
        {{-4,-2,2},{4,-2,2},{0,6,2},0},
    };
    assert(scene_open(walls,2));
    Hit hit={0};
    assert(scene_trace((Point){0,1,0},(Point){0,0,1},10,&hit));
    assert(fabsf(hit.distance-2)<.001f && hit.normal.z<-.99f);
    assert(!scene_trace((Point){0,1,0},(Point){0,0,-1},10,&hit));
    Player aiming={.position={3,1,-2},.yaw=.65f,.pitch=.22f};
    Camera sight={0};
    scene_scope(&sight,&aiming);
    assert(fabsf(sight.eye.x-3)<.001f && fabsf(sight.eye.y-2.52f)<.001f && fabsf(sight.eye.z+2)<.001f);
    assert(sight.distance==0 && fabsf(sight.forward.y-sinf(aiming.pitch))<.001f);
    assert(fabsf(atan2f(sight.forward.x,sight.forward.z)-aiming.yaw)<.001f);
    assert(fabsf(sight.right.x*sight.forward.x+sight.right.z*sight.forward.z)<.001f);
    Camera camera={.eye={0,1,0},.forward={0,0,1}};
    Effects fx={0};
    Point shoulder={0,1,0};
    assert(scene_shot(&fx,&camera,shoulder,(Point){.2f,1,.5f},&hit));
    assert(fabsf(hit.point.z-2)<.001f && fx.marks[0].life>0 && fx.flash>0);
    // A muzzle poking out past nearby cover must hit that cover.
    assert(scene_shot(&fx,&camera,shoulder,(Point){.2f,1,3},&hit));
    assert(fabsf(hit.point.z-2)<.001f);
    ShotTarget targets[]={{.feet={0,0,4},.active=true},{.feet={0,0,1.5f},.active=true}};
    int actor=-1;
    assert(scene_fire(&fx,&camera,shoulder,(Point){.02f,1,.4f},&hit,targets,2,&actor));
    assert(actor==1 && hit.point.z<1.5f);
    assert(scene_bodies((Point){0,.95f,1.5f},(Point){1,0,0},1,targets,2,&hit,&actor));
    assert(actor==1 && hit.distance==0 && hit.normal.x<-.99f);
    assert(scene_bodies((Point){.295f,.895f,0},(Point){0,0,1},2,targets,2,&hit,&actor));
    assert(actor==1 && hit.distance<1.5f);
    targets[1].active=false;
    assert(scene_fire(&fx,&camera,shoulder,(Point){.02f,1,.4f},&hit,targets,2,&actor));
    assert(actor==-1 && fabsf(hit.point.z-2)<.001f);
    scene_age(&fx,20);
    assert(fx.marks[0].life==0 && fx.flash==0);
    Player player={.position={0,0,0}};
    camera=(Camera){0};
    scene_camera(&camera,&player,0);
    assert(fabsf(camera.eye.z+2.25f)<.001f && fabsf(camera.eye.x-.62f)<.001f);
    player.yaw=3.14159265f;
    for (int i=0;i<80;i++) {
        scene_camera(&camera,&player,1.f/60);
        assert(camera.eye.z<2);
    }
    assert(camera.distance<2.1f && camera.distance>1);
    Camera track={.eye={0,1,5},.right={1,0,0},.up={0,1,0},.forward={0,0,-1}};
    scene_clip_camera(&track,(Point){0,1,0},scene_trace);
    assert(fabsf(track.eye.z-1.90f)<.001f && track.forward.z==-1);
    track.eye.z=1.20f;
    scene_clip_camera(&track,(Point){0,1,0},scene_trace);
    assert(fabsf(track.eye.z-1.20f)<.001f);
    player.position.z=-5;
    for (int i=0;i<80;i++) scene_camera(&camera,&player,1.f/60);
    assert(camera.distance>2.24f && camera.distance<2.36f);
    scene_close();
    // The Alley2 exit requires two successive jumps onto 1.4 m and 3.6 m surfaces.
    const Triangle steps[]={
        {{-8,0,-8},{8,0,-8},{0,0,12},1},
        {{0,0,-2},{0,1.4f,-2},{0,1.4f,2},0},{{0,0,-2},{0,1.4f,2},{0,0,2},0},
        {{0,1.4f,-2},{1.5f,1.4f,-2},{1.5f,1.4f,2},1},{{0,1.4f,-2},{1.5f,1.4f,2},{0,1.4f,2},1},
        {{2,0,-2},{2,3.6f,-2},{2,3.6f,2},0},{{2,0,-2},{2,3.6f,2},{2,0,2},0},
        {{2,3.6f,-2},{3,3.6f,-2},{3,3.6f,2},1},{{2,3.6f,-2},{3,3.6f,2},{2,3.6f,2},1}
    };
    assert(scene_open(steps,sizeof steps/sizeof *steps));
    player=(Player){.position={-.65f,0,0}};
    Jump jump={0};
    const float heights[]={1.4f,3.6f,0},goals[]={1,2.65f,3.65f};
    for (int leg=0;leg<3;leg++) {
        for (int frame=0;frame<120;frame++)
            scene_move_player(&player,&jump,fminf(3.4f/60,goals[leg]-player.position.x),0,1.f/60,!frame && leg<2);
        assert(!jump.airborne && fabsf(player.position.x-goals[leg])<.01f && fabsf(player.position.y-heights[leg])<.01f);
    }
    scene_close();
    // A capsule straddles a narrow train/platform gap without falling onto
    // the track. An actually unsupported body must still start falling.
    const Triangle sill[]={
        {{-2,0,-2},{0,0,-2},{0,0,2},1},{{-2,0,-2},{0,0,2},{-2,0,2},1},
        {{.44f,-.065f,-2},{3,-.065f,-2},{3,-.065f,2},1},{{.44f,-.065f,-2},{3,-.065f,2},{.44f,-.065f,2},1}
    };
    assert(scene_open(sill,4));
    for(unsigned fps=20;fps<=60;fps+=10){
        player=(Player){.position={-.5f,0,0}};Jump step={0};
        for(unsigned i=0;i<fps/2;i++){
            scene_move_gravity(&player,&step,4.f/fps,0,1.f/fps,false,20,7.5f);
            assert(!step.airborne && player.position.y>=-.066f);
        }
        assert(player.position.x>1.4f);
    }
    player=(Player){.position={3.5f,-.065f,0}};Jump unsupported={0};
    scene_move_gravity(&player,&unsupported,0,0,1.f/60,false,20,7.5f);assert(unsupported.airborne);
    scene_close();
    puts("Touch controls, native camera, hits, cover, consecutive jumps and landing passed.");
}
