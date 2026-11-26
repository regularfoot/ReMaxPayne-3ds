#include "gyro.h"
#include <3ds.h>
#include <math.h>

static aptHookCookie hook;
static bool requested, gyro_on, accel_on, fresh, have_gravity, resume_enabled;
static float counts_per_dps, down_y, down_z;

void gyro_set_active(bool active) {
    if (active==requested) return;
    requested=active;
    down_y=-1;down_z=0;have_gravity=false;fresh=true;
}
void gyro_prepare(bool enabled) {
    if (!enabled) {
        resume_enabled=false;
        gyro_set_active(false);
        if (accel_on) HIDUSER_DisableAccelerometer();
        if (gyro_on) HIDUSER_DisableGyroscope();
        accel_on=gyro_on=false;
        return;
    }
    if(gyro_on)return;
    gyro_on=R_SUCCEEDED(HIDUSER_EnableGyroscope());
    if (!gyro_on) return;
    if (R_FAILED(HIDUSER_GetGyroscopeRawToDpsCoefficient(&counts_per_dps)) ||
        !isfinite(counts_per_dps) || counts_per_dps<=0) counts_per_dps=14.375f;
    accel_on=R_SUCCEEDED(HIDUSER_EnableAccelerometer());
    down_y=-1;down_z=0;have_gravity=false;fresh=true;
}

static void suspend_motion(APT_HookType event,void *unused) {
    (void)unused;
    if (event==APTHOOK_ONSUSPEND || event==APTHOOK_ONSLEEP){
        bool restore=resume_enabled || gyro_on;gyro_prepare(false);resume_enabled=restore;
    }
    if(event==APTHOOK_ONRESTORE || event==APTHOOK_ONWAKEUP){
        if(resume_enabled)gyro_prepare(true);
        resume_enabled=false;
    }
    if(event==APTHOOK_ONEXIT){resume_enabled=false;gyro_prepare(false);}
}

void gyro_init(void) { aptHook(&hook,suspend_motion,NULL); }
void gyro_close(void) { aptUnhook(&hook);gyro_prepare(false); }

static float remove_drift(float rate) {
    const float deadzone=1.5f; // degrees per second, before sensitivity scaling
    return copysignf(fmaxf(0,fabsf(rate)-deadzone),rate);
}

GyroLook gyro_read(float seconds,unsigned horizontal_percent,unsigned vertical_percent) {
    GyroLook look={0};
    if (!requested || !gyro_on) return look;
    if (fresh) { fresh=false;return look; }
    if (accel_on) {
        accelVector a;
        hidAccelRead(&a);
        float length2=(float)a.x*a.x+(float)a.y*a.y+(float)a.z*a.z;
        // Gravity is about 512 counts. Ignore shakes and keep the last good vertical.
        if (length2>384.f*384.f && length2<640.f*640.f) {
            float weight=have_gravity?fminf(1,10*seconds):1;
            float scale=1/sqrtf(length2);
            down_y+=(a.y*scale-down_y)*weight;
            down_z+=(a.z*scale-down_z)*weight;
            have_gravity=true;
        }
    }
    angularRate rate;
    hidGyroRead(&rate);
    // libctru exposes gyro samples in X,Z,Y order and accelerometer samples in X,Y,Z.
    // Project the two non-hinge axes onto gravity so yaw follows a level turn at any playing angle.
    float yaw=(rate.z*down_y+rate.y*down_z)/counts_per_dps;
    float pitch=-rate.x/counts_per_dps;
    const float radians=0.01745329252f;
    look.yaw=remove_drift(yaw)*radians*seconds*(horizontal_percent*.01f);
    look.pitch=remove_drift(pitch)*radians*seconds*(vertical_percent*.01f);
    return look;
}
