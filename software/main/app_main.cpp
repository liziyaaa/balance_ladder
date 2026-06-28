#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "app_state.h"
#include "balance_controller.h"
#include "ble_debug.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "imu_mpu6050.h"
#include "motor_drv8837.h"
#include "nvs_flash.h"
#include "status_led.h"
#include "ui_oled.h"

namespace {

static const char *TAG = "balance_app";

static constexpr uint32_t kControlPeriodMs = 4;
static constexpr float kControlDtS = static_cast<float>(kControlPeriodMs) / 1000.0f;
static constexpr float kArmWindowDeg = 8.0f;
static constexpr uint32_t kAngleFaultDelayMs = 150;
static constexpr float kAngleFaultDeg = 35.0f;
static constexpr uint32_t kKeyDebounceMs = 35;
static constexpr uint32_t kKeyLongPressMs = 1500;

volatile bool s_calibrate_requested = false;
// motor override until (ms since boot). 0 means no override.
static uint32_t s_motor_override_until_ms = 0;
static float s_motor_override_cmd = 0.0f;
// 自动维持目标角的开关：开启时即使未手动 ARM，控制器也会输出驱动以维持 target
static bool s_auto_stabilize = false;

uint32_t millis()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

bool angle_near_target(const Telemetry &telemetry)
{
    return std::fabs(telemetry.error_deg) <= kArmWindowDeg;
}

bool can_arm(const Telemetry &telemetry)
{
    return angle_near_target(telemetry) && telemetry.imu_ok && telemetry.fault == FaultCode::NONE;
}

void force_motor_off()
{
    motor_coast();
    motor_sleep(true);
    balance_controller_reset();
}

void enter_disarmed()
{
    force_motor_off();
    app_state_set(AppState::DISARMED);
}

void enter_fault(FaultCode fault)
{
    force_motor_off();
    app_state_enter_fault(fault);
}

void trim_in_place(char *text)
{
    if (text == nullptr) {
        return;
    }

    size_t len = std::strlen(text);
    while (len > 0 && std::isspace(static_cast<unsigned char>(text[len - 1]))) {
        text[--len] = '\0';
    }

    char *start = text;
    while (*start != '\0' && std::isspace(static_cast<unsigned char>(*start))) {
        ++start;
    }
    if (start != text) {
        std::memmove(text, start, std::strlen(start) + 1);
    }
}

void lower_in_place(char *text)
{
    for (; text != nullptr && *text != '\0'; ++text) {
        *text = static_cast<char>(std::tolower(static_cast<unsigned char>(*text)));
    }
}

bool parse_float_strict(const char *text, float *value)
{
    if (text == nullptr || value == nullptr || *text == '\0') {
        return false;
    }

    char *end = nullptr;
    const float parsed = std::strtof(text, &end);
    if (end == text) {
        return false;
    }
    while (*end != '\0') {
        if (!std::isspace(static_cast<unsigned char>(*end))) {
            return false;
        }
        ++end;
    }
    *value = parsed;
    return std::isfinite(parsed);
}

bool command_value(const char *command, const char *prefix, float *value)
{
    const size_t prefix_len = std::strlen(prefix);
    if (std::strncmp(command, prefix, prefix_len) != 0) {
        return false;
    }
    return parse_float_strict(command + prefix_len, value);
}

void respond_status(char *response, size_t response_len)
{
    const Telemetry t = app_state_get_telemetry();
    const ControlParams p = app_state_get_params();
    std::snprintf(response, response_len,
                  "OK state=%s angle=%.2f target=%.2f kp=%.4f ki=%.4f kd=%.4f limit=%.2f gain_pos=%.3f gain_neg=%.3f min_pos=%.3f min_neg=%.3f kick_pos=%.3f kick_neg=%.3f baseline=%.3f auto=%d fault=%s\n",
                  app_state_name(t.state), t.angle_deg, p.target_angle_deg,
                  p.kp, p.ki, p.kd, p.output_limit,
                  p.gain_pos, p.gain_neg, p.min_pos, p.min_neg, p.kick_pos, p.kick_neg,
                  p.baseline_cmd, s_auto_stabilize ? 1 : 0, fault_code_name(t.fault));
}

void ble_command_handler(const char *command, char *response, size_t response_len)
{
    char cmd[96] {};
    std::strncpy(cmd, command, sizeof(cmd) - 1);
    trim_in_place(cmd);
    lower_in_place(cmd);

    if (cmd[0] == '\0') {
        std::snprintf(response, response_len, "ERR empty\n");
        return;
    }

    float value = 0.0f;
    if (parse_float_strict(cmd, &value)) {
        if (app_state_set_target(value)) {
            std::snprintf(response, response_len, "OK target=%.2f\n", value);
        } else {
            std::snprintf(response, response_len, "ERR target\n");
        }
        return;
    }

    if (command_value(cmd, "target=", &value)) {
        if (app_state_set_target(value)) {
            std::snprintf(response, response_len, "OK target=%.2f\n", value);
        } else {
            std::snprintf(response, response_len, "ERR target\n");
        }
        return;
    }

    if (command_value(cmd, "kp=", &value)) {
        ControlParams p = app_state_get_params();
        app_state_set_pid(value, p.ki, p.kd);
        std::snprintf(response, response_len, "OK kp=%.5f\n", value);
        return;
    }
    if (command_value(cmd, "ki=", &value)) {
        ControlParams p = app_state_get_params();
        app_state_set_pid(p.kp, value, p.kd);
        std::snprintf(response, response_len, "OK ki=%.5f\n", value);
        return;
    }
    if (command_value(cmd, "kd=", &value)) {
        ControlParams p = app_state_get_params();
        app_state_set_pid(p.kp, p.ki, value);
        std::snprintf(response, response_len, "OK kd=%.5f\n", value);
        return;
    }
    if (command_value(cmd, "limit=", &value)) {
        if (app_state_set_output_limit(value)) {
            std::snprintf(response, response_len, "OK limit=%.2f\n", value);
        } else {
            std::snprintf(response, response_len, "ERR range limit 0.05..1.00\n");
        }
        return;
    }
    if (command_value(cmd, "min_cmd=", &value)) {
        if (value < 0.0f || value > 1.0f) {
            std::snprintf(response, response_len, "ERR range min_cmd 0.0..1.0\n");
        } else {
            app_state_set_min_cmd(value);
            std::snprintf(response, response_len, "OK min_cmd=%.3f\n", value);
        }
        return;
    }
    if (command_value(cmd, "min_pos=", &value)) {
        if (value < 0.0f || value > 1.0f) {
            std::snprintf(response, response_len, "ERR range min_pos 0.0..1.0\n");
        } else {
            ControlParams p = app_state_get_params();
            app_state_set_motor_min_cmds(value, p.min_neg);
            std::snprintf(response, response_len, "OK min_pos=%.3f\n", value);
        }
        return;
    }
    if (command_value(cmd, "min_neg=", &value)) {
        if (value < 0.0f || value > 1.0f) {
            std::snprintf(response, response_len, "ERR range min_neg 0.0..1.0\n");
        } else {
            ControlParams p = app_state_get_params();
            app_state_set_motor_min_cmds(p.min_pos, value);
            std::snprintf(response, response_len, "OK min_neg=%.3f\n", value);
        }
        return;
    }
    if (command_value(cmd, "kick=", &value) || command_value(cmd, "kick_cmd=", &value)) {
        if (value < 0.0f || value > 1.0f) {
            std::snprintf(response, response_len, "ERR range kick 0.0..1.0\n");
        } else {
            app_state_set_kick_cmd(value);
            std::snprintf(response, response_len, "OK kick=%.3f\n", value);
        }
        return;
    }
    if (command_value(cmd, "kick_pos=", &value)) {
        if (value < 0.0f || value > 1.0f) {
            std::snprintf(response, response_len, "ERR range kick_pos 0.0..1.0\n");
        } else {
            ControlParams p = app_state_get_params();
            app_state_set_motor_kicks(value, p.kick_neg);
            std::snprintf(response, response_len, "OK kick_pos=%.3f\n", value);
        }
        return;
    }
    if (command_value(cmd, "kick_neg=", &value)) {
        if (value < 0.0f || value > 1.0f) {
            std::snprintf(response, response_len, "ERR range kick_neg 0.0..1.0\n");
        } else {
            ControlParams p = app_state_get_params();
            app_state_set_motor_kicks(p.kick_pos, value);
            std::snprintf(response, response_len, "OK kick_neg=%.3f\n", value);
        }
        return;
    }
    if (command_value(cmd, "gain=", &value)) {
        if (value < 0.0f || value > 3.0f) {
            std::snprintf(response, response_len, "ERR range gain 0.0..3.0\n");
        } else {
            app_state_set_motor_gains(value, value);
            std::snprintf(response, response_len, "OK gain=%.3f\n", value);
        }
        return;
    }
    if (command_value(cmd, "gain_pos=", &value)) {
        if (value < 0.0f || value > 3.0f) {
            std::snprintf(response, response_len, "ERR range gain_pos 0.0..3.0\n");
        } else {
            ControlParams p = app_state_get_params();
            app_state_set_motor_gains(value, p.gain_neg);
            std::snprintf(response, response_len, "OK gain_pos=%.3f\n", value);
        }
        return;
    }
    if (command_value(cmd, "gain_neg=", &value)) {
        if (value < 0.0f || value > 3.0f) {
            std::snprintf(response, response_len, "ERR range gain_neg 0.0..3.0\n");
        } else {
            ControlParams p = app_state_get_params();
            app_state_set_motor_gains(p.gain_pos, value);
            std::snprintf(response, response_len, "OK gain_neg=%.3f\n", value);
        }
        return;
    }
    if (command_value(cmd, "baseline=", &value)) {
        if (value < -1.0f || value > 1.0f) {
            std::snprintf(response, response_len, "ERR range baseline -1.0..1.0\n");
        } else {
            app_state_set_baseline_cmd(value);
            std::snprintf(response, response_len, "OK baseline=%.3f\n", value);
        }
        return;
    }

    if (std::strcmp(cmd, "stop") == 0) {
        enter_disarmed();
        std::snprintf(response, response_len, "OK stop\n");
        return;
    }
    if (std::strcmp(cmd, "arm") == 0) {
        if (app_state_fault() != FaultCode::NONE) {
            std::snprintf(response, response_len, "ERR fault %s\n", fault_code_name(app_state_fault()));
        } else {
            app_state_set(AppState::READY);
            std::snprintf(response, response_len, "OK arm wait_key\n");
        }
        return;
    }
    if (std::strcmp(cmd, "fault_clear") == 0) {
        app_state_clear_fault();
        balance_controller_reset();
        force_motor_off();
        std::snprintf(response, response_len, "OK fault_clear\n");
        return;
    }
    if (std::strcmp(cmd, "cal") == 0) {
        s_calibrate_requested = true;
        app_state_set(AppState::CALIBRATING);
        force_motor_off();
        std::snprintf(response, response_len, "OK cal\n");
        return;
    }
    if (std::strcmp(cmd, "level") == 0) {
        esp_err_t err = imu_set_current_angle(app_state_get_params().target_angle_deg);
        if (err == ESP_OK) {
            app_state_clear_fault();
            std::snprintf(response, response_len, "OK level current=target\n");
        } else {
            std::snprintf(response, response_len, "ERR level %s\n", esp_err_to_name(err));
        }
        return;
    }
    if (command_value(cmd, "level=", &value)) {
        if (value < 0.0f || value >= 360.0f) {
            std::snprintf(response, response_len, "ERR range level 0..359.99\n");
            return;
        }
        esp_err_t err = imu_set_current_angle(value);
        if (err == ESP_OK) {
            app_state_clear_fault();
            std::snprintf(response, response_len, "OK level=%.2f\n", value);
        } else {
            std::snprintf(response, response_len, "ERR level %s\n", esp_err_to_name(err));
        }
        return;
    }
    if (std::strcmp(cmd, "status") == 0) {
        respond_status(response, response_len);
        return;
    }
    if (std::strcmp(cmd, "defaults") == 0 || std::strcmp(cmd, "params_default") == 0) {
        app_state_reset_params();
        balance_controller_reset();
        respond_status(response, response_len);
        return;
    }

    if (std::strcmp(cmd, "auto_on") == 0) {
        s_auto_stabilize = true;
        std::snprintf(response, response_len, "OK auto_on\n");
        return;
    }
    if (std::strcmp(cmd, "auto_off") == 0) {
        s_auto_stabilize = false;
        std::snprintf(response, response_len, "OK auto_off\n");
        return;
    }
    if (command_value(cmd, "auto=", &value)) {
        s_auto_stabilize = (value != 0.0f);
        std::snprintf(response, response_len, "OK auto=%d\n", s_auto_stabilize ? 1 : 0);
        return;
    }

    if (std::strcmp(cmd, "force_wake_on") == 0) {
        motor_force_ignore_wake(true);
        std::snprintf(response, response_len, "OK force_wake_on\n");
        return;
    }
    if (std::strcmp(cmd, "force_wake_off") == 0) {
        motor_force_ignore_wake(false);
        std::snprintf(response, response_len, "OK force_wake_off\n");
        return;
    }
    if (command_value(cmd, "force_wake=", &value)) {
        motor_force_ignore_wake(value != 0.0f);
        std::snprintf(response, response_len, "OK force_wake=%d\n", value != 0.0f ? 1 : 0);
        return;
    }

    // Motor control test commands over BLE
    if (std::strcmp(cmd, "motor_full") == 0) {
        if (app_state_get() != AppState::DISARMED) {
            std::snprintf(response, response_len, "ERR state\n");
        } else {
            // start motor override for max 2000 ms
            const uint32_t now = millis();
            const uint32_t duration_ms = 2000;
            s_motor_override_until_ms = now + duration_ms;
            s_motor_override_cmd = 1.0f;
            motor_sleep(false);
            motor_set(s_motor_override_cmd);
            std::snprintf(response, response_len, "OK motor_full until=%lu\n", static_cast<unsigned long>(s_motor_override_until_ms));
        }
        return;
    }
    if (std::strcmp(cmd, "motor_full_rev") == 0) {
        if (app_state_get() != AppState::DISARMED) {
            std::snprintf(response, response_len, "ERR state\n");
            return;
        }
        const uint32_t now = millis();
        const uint32_t duration_ms = 2000;
        s_motor_override_until_ms = now + duration_ms;
        s_motor_override_cmd = -1.0f;
        motor_sleep(false);
        motor_set(s_motor_override_cmd);
        std::snprintf(response, response_len, "OK motor_full_rev until=%lu\n", static_cast<unsigned long>(s_motor_override_until_ms));
        return;
    }
    if (std::strcmp(cmd, "motor_stop") == 0) {
        motor_coast();
        motor_sleep(true);
        s_motor_override_until_ms = 0;
        s_motor_override_cmd = 0.0f;
        std::snprintf(response, response_len, "OK motor_stop\n");
        return;
    }
    if (command_value(cmd, "motor=", &value)) {
        if (value < -1.0f || value > 1.0f) {
            std::snprintf(response, response_len, "ERR range motor -1.0..1.0\n");
        } else {
            if (std::fabs(value) < 0.001f) {
                motor_coast();
                motor_sleep(true);
                s_motor_override_until_ms = 0;
                s_motor_override_cmd = 0.0f;
            } else {
                if (app_state_get() != AppState::DISARMED) {
                    std::snprintf(response, response_len, "ERR state\n");
                    return;
                }
                const uint32_t now = millis();
                const uint32_t duration_ms = 5000;
                s_motor_override_until_ms = now + duration_ms;
                s_motor_override_cmd = value;
                motor_sleep(false);
                motor_set(value);
            }
            std::snprintf(response, response_len, "OK motor=%.3f\n", value);
        }
        return;
    }

    std::snprintf(response, response_len, "ERR cmd\n");
}

void handle_short_press()
{
    const Telemetry telemetry = app_state_get_telemetry();
    if (telemetry.state == AppState::ARMED) {
        enter_disarmed();
        return;
    }

    if (telemetry.state != AppState::DISARMED && telemetry.state != AppState::READY) {
        return;
    }

    if (telemetry.imu_ok && telemetry.fault == FaultCode::NONE) {
        const esp_err_t err = imu_set_current_angle(telemetry.target_deg);
        if (err != ESP_OK) {
            enter_fault(FaultCode::IMU);
            ESP_LOGE(TAG, "key level failed: %s", esp_err_to_name(err));
            return;
        }
        balance_controller_reset();
        motor_sleep(false);
        app_state_set(AppState::ARMED);
        ESP_LOGI(TAG, "armed by key, current posture leveled to %.2f deg", telemetry.target_deg);
        return;
    }

    ESP_LOGW(TAG, "key ignored: state=%s angle=%.2f target=%.2f error=%.2f imu=%d fault=%s",
             app_state_name(telemetry.state),
             telemetry.angle_deg,
             telemetry.target_deg,
             telemetry.error_deg,
             telemetry.imu_ok ? 1 : 0,
             fault_code_name(telemetry.fault));
}

void handle_long_press()
{
    enter_fault(FaultCode::KEY_LONG);
}

void control_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t angle_fault_since_ms = 0;

    while (true) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kControlPeriodMs));
        const uint32_t now = millis();

        if (s_calibrate_requested) {
            app_state_set(AppState::CALIBRATING);
            force_motor_off();
            esp_err_t err = imu_calibrate();
            s_calibrate_requested = false;
            if (err == ESP_OK) {
                app_state_clear_fault();
                app_state_set(AppState::DISARMED);
            } else {
                enter_fault(FaultCode::IMU);
            }
            continue;
        }

        ImuReading imu {};
        esp_err_t imu_err = imu_update(kControlDtS, &imu);
        const bool imu_ok = imu_err == ESP_OK && imu.ok;
        ControlParams params = app_state_get_params();
        AppState state = app_state_get();
        Telemetry before = app_state_get_telemetry();
        float cmd = 0.0f;

        if (!imu_ok) {
            app_state_set_motion(before.angle_deg, 0.0f, 0.0f, false);
            if (state == AppState::ARMED) {
                enter_fault(FaultCode::IMU);
            }
            continue;
        }

        const float measured_angle = imu.angle_deg;
        const float error = app_angle_error_deg(params.target_angle_deg, measured_angle);
        if (state == AppState::FAULT) {
            force_motor_off();
            app_state_set_motion(measured_angle, imu.gyro_rate_deg_s, 0.0f, true, imu.accel_plane_deg, imu.accel_angle_deg);
            continue;
        }

        if (state == AppState::ARMED || s_auto_stabilize) {
            if (std::fabs(error) > kAngleFaultDeg) {
                if (angle_fault_since_ms == 0) {
                    angle_fault_since_ms = now;
                } else if (now - angle_fault_since_ms >= kAngleFaultDelayMs) {
                    enter_fault(FaultCode::ANGLE_LIMIT);
                    app_state_set_motion(measured_angle, imu.gyro_rate_deg_s, 0.0f, true, imu.accel_plane_deg, imu.accel_angle_deg);
                    angle_fault_since_ms = 0;
                    continue;
                }
            } else {
                angle_fault_since_ms = 0;
            }

            cmd = balance_controller_update(measured_angle, imu.gyro_rate_deg_s, params, kControlDtS);
            // Add baseline command (user-configurable) before sending to motor
            {
                float final_cmd = cmd + params.baseline_cmd;
                // enforce output limit
                if (final_cmd > params.output_limit) final_cmd = params.output_limit;
                if (final_cmd < -params.output_limit) final_cmd = -params.output_limit;
                cmd = motor_set(final_cmd);
            }
        } else {
            angle_fault_since_ms = 0;
            const uint32_t now = millis();
            if (s_motor_override_until_ms != 0) {
                if (now >= s_motor_override_until_ms) {
                    // override expired
                    s_motor_override_until_ms = 0;
                    s_motor_override_cmd = 0.0f;
                    motor_coast();
                    motor_sleep(true);
                } else {
                    // keep override active
                    motor_sleep(false);
                    const float actual_cmd = motor_set(s_motor_override_cmd);
                    app_state_set_motion(measured_angle, imu.gyro_rate_deg_s, actual_cmd, true, imu.accel_plane_deg, imu.accel_angle_deg);
                    continue;
                }
            } else {
                force_motor_off();
            }

            Telemetry temp = before;
            temp.angle_deg = measured_angle;
            temp.target_deg = params.target_angle_deg;
            temp.error_deg = error;
            temp.imu_ok = true;
            if ((state == AppState::DISARMED || state == AppState::READY) && can_arm(temp)) {
                app_state_set(AppState::READY);
            } else if (state == AppState::READY) {
                app_state_set(AppState::DISARMED);
            }
        }

        app_state_set_motion(measured_angle, imu.gyro_rate_deg_s, cmd, true, imu.accel_plane_deg, imu.accel_angle_deg);
    }
}

void key_task(void *arg)
{
    (void)arg;

    gpio_config_t cfg {};
    cfg.pin_bit_mask = 1ULL << static_cast<uint32_t>(PIN_ARM_KEY);
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&cfg));

    bool raw_pressed = gpio_get_level(PIN_ARM_KEY) == 0;
    bool last_raw = raw_pressed;
    bool debounced = raw_pressed;
    bool long_sent = false;
    uint32_t last_change_ms = millis();
    uint32_t press_start_ms = raw_pressed ? millis() : 0;
    app_state_set_key_pressed(debounced);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10));
        const uint32_t now = millis();
        raw_pressed = gpio_get_level(PIN_ARM_KEY) == 0;

        if (raw_pressed != last_raw) {
            last_raw = raw_pressed;
            last_change_ms = now;
        }

        if ((now - last_change_ms) >= kKeyDebounceMs && raw_pressed != debounced) {
            debounced = raw_pressed;
            app_state_set_key_pressed(debounced);

            if (debounced) {
                press_start_ms = now;
                long_sent = false;
            } else {
                const uint32_t duration = now - press_start_ms;
                if (!long_sent && duration < kKeyLongPressMs) {
                    handle_short_press();
                }
            }
        }

        if (debounced && !long_sent && (now - press_start_ms) >= kKeyLongPressMs) {
            long_sent = true;
            handle_long_press();
        }
    }
}

void ble_notify_task(void *arg)
{
    (void)arg;
    while (true) {
        Telemetry telemetry = app_state_get_telemetry();
        telemetry.t_ms = millis();
        ble_debug_notify_telemetry(telemetry);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void ui_task(void *arg)
{
    (void)arg;
    while (true) {
        ui_oled_update(app_state_get_telemetry());
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void led_task(void *arg)
{
    (void)arg;
    while (true) {
        status_led_update(app_state_get(), millis());
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

void diagnostic_task(void *arg)
{
    (void)arg;
    bool sleep_read_warned = false;
    while (true) {
        const Telemetry t = app_state_get_telemetry();
        const MotorDebug motor = motor_debug_get();
        if (motor.sleep_set_level == 1 && motor.sleep_read_level == 0 &&
            (motor.in1_duty > 0 || motor.in2_duty > 0) && !sleep_read_warned) {
            ESP_LOGW(TAG, "GPIO5 nSLEEP was written HIGH but readback is LOW; trust meter reading first and check actual DRV input/output voltage");
            sleep_read_warned = true;
        } else if (motor.sleep_read_level == 1) {
            sleep_read_warned = false;
        }
        ESP_LOGI(TAG, "diag state=%s angle=%.2f target=%.2f error=%.2f gyro_y=%.2f accel_plane=%.2f accel_angle=%.2f cmd=%.3f drv_slp_set=%d drv_slp_read=%d wake_fail=%d in1=%lu/%lu in2=%lu/%lu key=%d ble=%d fault=%s",
                 app_state_name(t.state),
                 t.angle_deg,
                 t.target_deg,
                 t.error_deg,
                 t.gyro_rate_deg_s,
                 t.accel_plane_deg,
                 t.accel_angle_deg,
                 t.motor_cmd,
                 motor.sleep_set_level,
             motor.sleep_read_level,
             motor.wake_failed ? 1 : 0,
                 static_cast<unsigned long>(motor.in1_duty),
                 static_cast<unsigned long>(motor.max_duty),
                 static_cast<unsigned long>(motor.in2_duty),
                 static_cast<unsigned long>(motor.max_duty),
                 t.key_pressed ? 1 : 0,
                 t.ble_connected ? 1 : 0,
                 fault_code_name(t.fault));
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

esp_err_t init_nvs()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

} // namespace

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(init_nvs());
    app_state_init();

    ESP_ERROR_CHECK(status_led_init());
    status_led_update(AppState::BOOT, millis());

    ESP_ERROR_CHECK(motor_init());
    force_motor_off();

    // Startup motor diagnostic removed to avoid spinning motor before user ARM.

    esp_err_t oled_err = ui_oled_init();
    if (oled_err != ESP_OK) {
        ESP_LOGW(TAG, "OLED init failed: %s", esp_err_to_name(oled_err));
    }

    esp_err_t imu_err = imu_init();
    if (imu_err != ESP_OK) {
        ESP_LOGE(TAG, "MPU6050 init failed: %s", esp_err_to_name(imu_err));
        app_state_enter_fault(FaultCode::IMU);
    } else {
        app_state_set(AppState::CALIBRATING);
        imu_err = imu_calibrate();
        if (imu_err == ESP_OK) {
            app_state_set(AppState::DISARMED);
        } else {
            ESP_LOGE(TAG, "MPU6050 calibration failed: %s", esp_err_to_name(imu_err));
            app_state_enter_fault(FaultCode::IMU);
        }
    }

    esp_err_t ble_err = ble_debug_init(ble_command_handler);
    if (ble_err != ESP_OK) {
        ESP_LOGW(TAG, "BLE init failed: %s", esp_err_to_name(ble_err));
    }

    xTaskCreate(control_task, "control_task", 4096, nullptr, 10, nullptr);
    xTaskCreate(key_task, "key_task", 3072, nullptr, 8, nullptr);
    xTaskCreate(ble_notify_task, "ble_notify_task", 4096, nullptr, 4, nullptr);
    xTaskCreate(ui_task, "ui_task", 4096, nullptr, 3, nullptr);
    xTaskCreate(led_task, "led_task", 2048, nullptr, 3, nullptr);
    xTaskCreate(diagnostic_task, "diagnostic_task", 4096, nullptr, 2, nullptr);

    ESP_LOGI(TAG, "Balance ladder firmware started, console uses USB Serial/JTAG");
}
