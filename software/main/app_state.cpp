#include "app_state.h"

#include <cmath>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "nvs.h"
#include "esp_log.h"
#include "esp_err.h"

namespace {

SemaphoreHandle_t s_lock = nullptr;
AppState s_state = AppState::BOOT;
FaultCode s_fault = FaultCode::NONE;
ControlParams s_params;
Telemetry s_telemetry;

void lock()
{
    if (s_lock != nullptr) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

void unlock()
{
    if (s_lock != nullptr) {
        xSemaphoreGive(s_lock);
    }
}

float clampf(float value, float lo, float hi)
{
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

float wrap_360(float angle_deg)
{
    while (angle_deg >= 360.0f) {
        angle_deg -= 360.0f;
    }
    while (angle_deg < 0.0f) {
        angle_deg += 360.0f;
    }
    return angle_deg;
}

} // namespace

float app_angle_error_deg(float target_deg, float measured_deg)
{
    float error = wrap_360(target_deg) - wrap_360(measured_deg);
    if (error > 180.0f) {
        error -= 360.0f;
    } else if (error < -180.0f) {
        error += 360.0f;
    }
    return error;
}

const char *app_state_name(AppState state)
{
    switch (state) {
    case AppState::BOOT:
        return "BOOT";
    case AppState::CALIBRATING:
        return "CALIBRATING";
    case AppState::DISARMED:
        return "DISARMED";
    case AppState::READY:
        return "READY";
    case AppState::ARMED:
        return "ARMED";
    case AppState::FAULT:
        return "FAULT";
    default:
        return "UNKNOWN";
    }
}

const char *fault_code_name(FaultCode fault)
{
    switch (fault) {
    case FaultCode::NONE:
        return "NONE";
    case FaultCode::ANGLE_LIMIT:
        return "ANGLE_LIMIT";
    case FaultCode::IMU:
        return "IMU";
    case FaultCode::I2C:
        return "I2C";
    case FaultCode::USER_STOP:
        return "USER_STOP";
    case FaultCode::KEY_LONG:
        return "KEY_LONG";
    default:
        return "UNKNOWN";
    }
}

void app_state_init()
{
    s_lock = xSemaphoreCreateMutex();
    lock();
    s_state = AppState::BOOT;
    s_fault = FaultCode::NONE;
    s_params = ControlParams {};
    s_telemetry = Telemetry {};
    s_telemetry.state = s_state;
    s_telemetry.fault = s_fault;
    s_telemetry.target_deg = s_params.target_angle_deg;
    // Attempt to load persisted control parameters from NVS
    {
        nvs_handle_t handle;
        esp_err_t rc = nvs_open("storage", NVS_READONLY, &handle);
        if (rc == ESP_OK) {
            size_t required = 0;
            rc = nvs_get_blob(handle, "params", nullptr, &required);
            if (rc == ESP_OK && required == sizeof(s_params)) {
                rc = nvs_get_blob(handle, "params", &s_params, &required);
                if (rc == ESP_OK) {
                            ESP_LOGI("app_state", "Loaded persisted params kp=%.6f ki=%.6f kd=%.6f limit=%.3f min_cmd=%.3f baseline=%.3f target=%.2f",
                                     s_params.kp, s_params.ki, s_params.kd, s_params.output_limit, s_params.min_cmd, s_params.baseline_cmd, s_params.target_angle_deg);
                    s_telemetry.target_deg = s_params.target_angle_deg;
                } else {
                    ESP_LOGW("app_state", "Failed to read params blob: %d", rc);
                }
            }
            nvs_close(handle);
        }
    }
    unlock();
}

AppState app_state_get()
{
    lock();
    AppState state = s_state;
    unlock();
    return state;
}

void app_state_set(AppState state)
{
    lock();
    s_state = state;
    if (state != AppState::FAULT && s_fault == FaultCode::NONE) {
        s_telemetry.fault = FaultCode::NONE;
    }
    s_telemetry.state = s_state;
    unlock();
}

void app_state_enter_fault(FaultCode fault)
{
    lock();
    s_state = AppState::FAULT;
    s_fault = fault;
    s_telemetry.state = s_state;
    s_telemetry.fault = s_fault;
    unlock();
}

void app_state_clear_fault()
{
    lock();
    s_fault = FaultCode::NONE;
    s_state = AppState::DISARMED;
    s_telemetry.state = s_state;
    s_telemetry.fault = s_fault;
    unlock();
}

FaultCode app_state_fault()
{
    lock();
    FaultCode fault = s_fault;
    unlock();
    return fault;
}

ControlParams app_state_get_params()
{
    lock();
    ControlParams params = s_params;
    unlock();
    return params;
}

bool app_state_set_target(float target_deg)
{
    // Accept any finite angle in 0..359.99 degrees. Wrap values into 0..360.
    if (!std::isfinite(target_deg)) {
        return false;
    }
    // Normalize into 0..360 range
    float norm = target_deg;
    while (norm >= 360.0f) norm -= 360.0f;
    while (norm < 0.0f) norm += 360.0f;

    // Restrict operational target to 250..290 degrees per user requirement
    if (norm < 250.0f) norm = 250.0f;
    if (norm > 290.0f) norm = 290.0f;

    lock();
    s_params.target_angle_deg = norm;
    s_telemetry.target_deg = norm;
    s_telemetry.error_deg = app_angle_error_deg(s_params.target_angle_deg, s_telemetry.angle_deg);
    unlock();
    // persist target
    {
        nvs_handle_t handle;
        esp_err_t rc = nvs_open("storage", NVS_READWRITE, &handle);
        if (rc == ESP_OK) {
            rc = nvs_set_blob(handle, "params", &s_params, sizeof(s_params));
            if (rc == ESP_OK) rc = nvs_commit(handle);
            if (rc != ESP_OK) ESP_LOGW("app_state", "Failed to save target: %d", rc);
            nvs_close(handle);
        }
    }
    return true;
}

void app_state_set_pid(float kp, float ki, float kd)
{
    lock();
    s_params.kp = kp;
    s_params.ki = ki;
    s_params.kd = kd;
    unlock();
    // Persist params
    {
        nvs_handle_t handle;
        esp_err_t rc = nvs_open("storage", NVS_READWRITE, &handle);
        if (rc == ESP_OK) {
            rc = nvs_set_blob(handle, "params", &s_params, sizeof(s_params));
            if (rc == ESP_OK) {
                rc = nvs_commit(handle);
            }
            if (rc != ESP_OK) {
                ESP_LOGW("app_state", "Failed to save params to NVS: %d", rc);
            } else {
                ESP_LOGI("app_state", "Saved params kp=%.6f ki=%.6f kd=%.6f limit=%.3f min_cmd=%.3f baseline=%.3f target=%.2f",
                         s_params.kp, s_params.ki, s_params.kd, s_params.output_limit, s_params.min_cmd, s_params.baseline_cmd, s_params.target_angle_deg);
            }
            nvs_close(handle);
        } else {
            ESP_LOGW("app_state", "NVS open failed when saving params: %d", rc);
        }
    }
}

void app_state_set_kp(float kp)
{
    lock();
    s_params.kp = kp;
    unlock();
    // persist kp
    {
        nvs_handle_t handle;
        esp_err_t rc = nvs_open("storage", NVS_READWRITE, &handle);
        if (rc == ESP_OK) {
            rc = nvs_set_blob(handle, "params", &s_params, sizeof(s_params));
            if (rc == ESP_OK) rc = nvs_commit(handle);
            if (rc != ESP_OK) ESP_LOGW("app_state", "Failed to save kp: %d", rc);
            nvs_close(handle);
        }
    }
}

void app_state_set_ki(float ki)
{
    lock();
    s_params.ki = ki;
    unlock();
    // persist ki
    {
        nvs_handle_t handle;
        esp_err_t rc = nvs_open("storage", NVS_READWRITE, &handle);
        if (rc == ESP_OK) {
            rc = nvs_set_blob(handle, "params", &s_params, sizeof(s_params));
            if (rc == ESP_OK) rc = nvs_commit(handle);
            if (rc != ESP_OK) ESP_LOGW("app_state", "Failed to save ki: %d", rc);
            nvs_close(handle);
        }
    }
}

void app_state_set_kd(float kd)
{
    lock();
    s_params.kd = kd;
    unlock();
    // persist kd
    {
        nvs_handle_t handle;
        esp_err_t rc = nvs_open("storage", NVS_READWRITE, &handle);
        if (rc == ESP_OK) {
            rc = nvs_set_blob(handle, "params", &s_params, sizeof(s_params));
            if (rc == ESP_OK) rc = nvs_commit(handle);
            if (rc != ESP_OK) ESP_LOGW("app_state", "Failed to save kd: %d", rc);
            nvs_close(handle);
        }
    }
}

bool app_state_set_output_limit(float limit)
{
    if (!std::isfinite(limit) || limit < 0.05f || limit > 1.0f) {
        return false;
    }

    lock();
    s_params.output_limit = clampf(limit, 0.05f, 1.0f);
    unlock();
    // persist output limit
    {
        nvs_handle_t handle;
        esp_err_t rc = nvs_open("storage", NVS_READWRITE, &handle);
        if (rc == ESP_OK) {
            rc = nvs_set_blob(handle, "params", &s_params, sizeof(s_params));
            if (rc == ESP_OK) rc = nvs_commit(handle);
            if (rc != ESP_OK) ESP_LOGW("app_state", "Failed to save output_limit: %d", rc);
            nvs_close(handle);
        }
    }
    return true;
}

void app_state_set_min_cmd(float min_cmd)
{
    lock();
    s_params.min_cmd = min_cmd;
    unlock();
    // persist min_cmd
    {
        nvs_handle_t handle;
        esp_err_t rc = nvs_open("storage", NVS_READWRITE, &handle);
        if (rc == ESP_OK) {
            rc = nvs_set_blob(handle, "params", &s_params, sizeof(s_params));
            if (rc == ESP_OK) rc = nvs_commit(handle);
            if (rc != ESP_OK) ESP_LOGW("app_state", "Failed to save min_cmd: %d", rc);
            nvs_close(handle);
        }
    }
}

void app_state_set_baseline_cmd(float baseline)
{
    lock();
    s_params.baseline_cmd = baseline;
    unlock();
    // persist baseline
    {
        nvs_handle_t handle;
        esp_err_t rc = nvs_open("storage", NVS_READWRITE, &handle);
        if (rc == ESP_OK) {
            rc = nvs_set_blob(handle, "params", &s_params, sizeof(s_params));
            if (rc == ESP_OK) rc = nvs_commit(handle);
            if (rc != ESP_OK) ESP_LOGW("app_state", "Failed to save baseline_cmd: %d", rc);
            nvs_close(handle);
        }
    }
}

void app_state_reset_params()
{
    lock();
    s_params = ControlParams {};
    s_telemetry.target_deg = s_params.target_angle_deg;
    s_telemetry.error_deg = app_angle_error_deg(s_params.target_angle_deg, s_telemetry.angle_deg);
    const ControlParams params = s_params;
    unlock();

    nvs_handle_t handle;
    esp_err_t rc = nvs_open("storage", NVS_READWRITE, &handle);
    if (rc == ESP_OK) {
        rc = nvs_set_blob(handle, "params", &params, sizeof(params));
        if (rc == ESP_OK) {
            rc = nvs_commit(handle);
        }
        if (rc != ESP_OK) {
            ESP_LOGW("app_state", "Failed to save default params: %d", rc);
        } else {
            ESP_LOGI("app_state", "Reset params to defaults kp=%.6f ki=%.6f kd=%.6f limit=%.3f min_cmd=%.3f baseline=%.3f target=%.2f",
                     params.kp, params.ki, params.kd, params.output_limit, params.min_cmd, params.baseline_cmd, params.target_angle_deg);
        }
        nvs_close(handle);
    } else {
        ESP_LOGW("app_state", "NVS open failed when resetting params: %d", rc);
    }
}

void app_state_set_key_pressed(bool pressed)
{
    lock();
    s_telemetry.key_pressed = pressed;
    unlock();
}

void app_state_set_ble_connected(bool connected)
{
    lock();
    s_telemetry.ble_connected = connected;
    unlock();
}

void app_state_set_motion(float angle_deg,
                          float gyro_rate_deg_s,
                          float motor_cmd,
                          bool imu_ok,
                          float accel_plane_deg,
                          float accel_angle_deg)
{
    lock();
    s_telemetry.angle_deg = angle_deg;
    s_telemetry.target_deg = s_params.target_angle_deg;
    s_telemetry.error_deg = app_angle_error_deg(s_params.target_angle_deg, angle_deg);
    s_telemetry.gyro_rate_deg_s = gyro_rate_deg_s;
    s_telemetry.accel_plane_deg = accel_plane_deg;
    s_telemetry.accel_angle_deg = accel_angle_deg;
    s_telemetry.motor_cmd = motor_cmd;
    s_telemetry.imu_ok = imu_ok;
    unlock();
}

Telemetry app_state_get_telemetry()
{
    lock();
    Telemetry telemetry = s_telemetry;
    telemetry.state = s_state;
    telemetry.fault = s_fault;
    telemetry.target_deg = s_params.target_angle_deg;
    telemetry.error_deg = app_angle_error_deg(s_params.target_angle_deg, telemetry.angle_deg);
    unlock();
    return telemetry;
}
