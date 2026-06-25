#pragma once

#include <cstdint>

#include "esp_err.h"

struct MotorDebug {
    float cmd = 0.0f;
    uint32_t in1_duty = 0;
    uint32_t in2_duty = 0;
    uint32_t max_duty = 0;
    int sleep_set_level = 0;
    int sleep_read_level = 0;
    bool wake_failed = false;
};

esp_err_t motor_init();
void motor_set(float cmd);
void motor_coast();
void motor_brake();
void motor_sleep(bool sleep);
MotorDebug motor_debug_get();
// Force ignore wake failure (for testing). When set true, motor_set will
// attempt to drive even if readback shows sleep low. Use with caution.
void motor_force_ignore_wake(bool ignore);
