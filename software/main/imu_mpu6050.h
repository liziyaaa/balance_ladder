#pragma once

#include "esp_err.h"

struct ImuReading {
    float angle_deg = 90.0f;
    float accel_angle_deg = 90.0f;
    float accel_plane_deg = 0.0f;
    float gyro_rate_deg_s = 0.0f;
    bool ok = false;
};

esp_err_t imu_init();
esp_err_t imu_calibrate();
esp_err_t imu_set_current_angle(float desired_angle_deg);
bool imu_set_accel_axes(char angle_axis, char reference_axis);
void imu_get_accel_axes(char *angle_axis, char *reference_axis);
esp_err_t imu_update(float dt_s, ImuReading *reading);
