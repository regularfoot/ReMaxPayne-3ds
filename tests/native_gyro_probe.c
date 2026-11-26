#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <3ds.h>
typedef int APT_HookType;
typedef int aptHookCookie;
enum {APTHOOK_ONSUSPEND,APTHOOK_ONRESTORE,APTHOOK_ONSLEEP,APTHOOK_ONWAKEUP,APTHOOK_ONEXIT};
typedef struct {int x,y,z;} accelVector,angularRate;
#define R_SUCCEEDED(x) ((x)>=0)
static unsigned enables,disables,coefficients,accel_enables,accel_disables;
static int enable_result;
static void aptHook(aptHookCookie *c,void (*f)(APT_HookType,void*),void *arg){(void)c;(void)f;(void)arg;}
static void aptUnhook(aptHookCookie *c){(void)c;}
static int HIDUSER_EnableGyroscope(void){enables++;return enable_result;}
static int HIDUSER_DisableGyroscope(void){disables++;return 0;}
static int HIDUSER_EnableAccelerometer(void){accel_enables++;return 0;}
static int HIDUSER_DisableAccelerometer(void){accel_disables++;return 0;}
static int HIDUSER_GetGyroscopeRawToDpsCoefficient(float *value){coefficients++;*value=14.375f;return 0;}
static void hidAccelRead(accelVector *v){*v=(accelVector){0,-512,0};}
static void hidGyroRead(angularRate *v){*v=(angularRate){100,0,100};}
#include "../source/native/gyro.c"
int main(void){
    gyro_init();gyro_set_active(true);assert(enables==0);assert(gyro_read(.02f,100,100).yaw==0);
    gyro_set_active(false);gyro_prepare(true);assert(enables==1 && coefficients==1 && accel_enables==1);
    assert(gyro_read(.02f,100,100).yaw==0); // Prepared hardware is not camera input during a movie.
    gyro_set_active(true);assert(gyro_read(.02f,100,100).yaw==0); // First sample is discarded.
    GyroLook look=gyro_read(.02f,100,100);assert(isfinite(look.yaw) && look.yaw!=0 && look.pitch!=0);
    for(unsigned frame=0;frame<120;frame++){
        gyro_set_active(false);assert(gyro_read(.02f,100,100).yaw==0);gyro_set_active(true);
    }
    gyro_prepare(true);assert(enables==1 && disables==0 && coefficients==1 && accel_enables==1);
    suspend_motion(APTHOOK_ONSLEEP,NULL);suspend_motion(APTHOOK_ONSUSPEND,NULL);assert(disables==1 && accel_disables==1);
    suspend_motion(APTHOOK_ONWAKEUP,NULL);suspend_motion(APTHOOK_ONRESTORE,NULL);assert(enables==2 && gyro_on);
    gyro_set_active(true);gyro_read(.02f,100,100);assert(gyro_read(.02f,100,100).yaw!=0);
    gyro_prepare(false);assert(disables==2 && !gyro_on);gyro_set_active(true);assert(enables==2);
    gyro_set_active(false);enable_result=-1;gyro_prepare(true);assert(enables==3 && !gyro_on);gyro_set_active(true);
    for(unsigned frame=0;frame<120;frame++)assert(gyro_read(.02f,100,100).yaw==0);
    assert(enables==3);enable_result=0;gyro_prepare(true);assert(enables==4 && gyro_on);gyro_close();
    puts("Gyro: loading-only IPC, cinematic input gate, resume, disable and failed sensor verified");return 0;
}
