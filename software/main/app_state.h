#pragma once

#include <cstddef>
#include <cstdint>

enum class AppState : uint8_t {
    BOOT = 0,
    CALIBRATING,
    DISARMED,
    READY,
    ARMED,
    FAULT,
};

enum class FaultCode : uint8_t {
    NONE = 0,
    ANGLE_LIMIT,
    IMU,
    I2C,
    USER_STOP,
    KEY_LONG,
};

struct ControlParams {
    float target_angle_deg = 270.0f;
    float kp = 0.02f;
    float ki = 0.0f;
    float kd = 0.004f;
    float output_limit = 1.0f;
    float min_cmd = 0.12f; // minimum absolute motor command to overcome deadzone
    float baseline_cmd = 0.0f; // baseline motor command added to controller output
};

struct Telemetry {
    uint32_t t_ms = 0;
    AppState state = AppState::BOOT;
    FaultCode fault = FaultCode::NONE;
    float angle_deg = 90.0f;
    float target_deg = 270.0f;
    float error_deg = 0.0f;
    float gyro_rate_deg_s = 0.0f;
    float accel_plane_deg = 0.0f;
    float accel_angle_deg = 90.0f;
    float motor_cmd = 0.0f;
    bool key_pressed = false;
    bool ble_connected = false;
    bool imu_ok = false;
};

const char *app_state_name(AppState state);
const char *fault_code_name(FaultCode fault);
float app_angle_error_deg(float target_deg, float measured_deg);

void app_state_init();

AppState app_state_get();
void app_state_set(AppState state);
void app_state_enter_fault(FaultCode fault);
void app_state_clear_fault();
FaultCode app_state_fault();

ControlParams app_state_get_params();
bool app_state_set_target(float target_deg);
void app_state_set_pid(float kp, float ki, float kd);
void app_state_set_kp(float kp);
void app_state_set_ki(float ki);
void app_state_set_kd(float kd);
bool app_state_set_output_limit(float limit);
void app_state_set_min_cmd(float min_cmd);
void app_state_set_baseline_cmd(float baseline);
void app_state_reset_params();

void app_state_set_key_pressed(bool pressed);
void app_state_set_ble_connected(bool connected);
void app_state_set_motion(float angle_deg,
                          float gyro_rate_deg_s,
                          float motor_cmd,
                          bool imu_ok,
                          float accel_plane_deg = 0.0f,
                          float accel_angle_deg = 90.0f);
Telemetry app_state_get_telemetry();
