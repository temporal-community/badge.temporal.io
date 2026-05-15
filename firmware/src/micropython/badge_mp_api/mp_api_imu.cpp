#include "../../hardware/IMU.h"

#include "temporalbadge_runtime.h"

extern IMU imu;

extern "C" int temporalbadge_runtime_imu_ready(void)
{
    return imu.isReady() ? 1 : 0;
}

extern "C" float temporalbadge_runtime_imu_tilt_x(void)
{
    imu.service();
    return imu.tiltXMg();
}

extern "C" float temporalbadge_runtime_imu_tilt_y(void)
{
    imu.service();
    return imu.tiltYMg();
}

extern "C" float temporalbadge_runtime_imu_accel_z(void)
{
    imu.service();
    return imu.accelZMg();
}

extern "C" int temporalbadge_runtime_imu_face_down(void)
{
    imu.service();
    return imu.isFaceDown() ? 1 : 0;
}

extern "C" int temporalbadge_runtime_imu_motion(void)
{
    imu.service();
    return imu.consumeMotionEvent() ? 1 : 0;
}
