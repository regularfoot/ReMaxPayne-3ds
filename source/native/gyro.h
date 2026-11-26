#ifndef MP_NATIVE_GYRO_H
#define MP_NATIVE_GYRO_H
#include <stdbool.h>

typedef struct { float yaw, pitch; } GyroLook;

void gyro_set_active(bool active);
// Sensor IPC may wait on hardware. Call while loading or resuming a menu.
void gyro_prepare(bool enabled);
GyroLook gyro_read(float seconds,unsigned horizontal_percent,unsigned vertical_percent);
void gyro_init(void);
void gyro_close(void);
#endif
