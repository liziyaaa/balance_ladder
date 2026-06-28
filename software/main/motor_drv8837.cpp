#include "motor_drv8837.h"

#include <cmath>

#include "board_pins.h"
#include "app_state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_timer.h"

namespace {

static constexpr ledc_mode_t kLedcMode = LEDC_LOW_SPEED_MODE;
static constexpr ledc_timer_t kLedcTimer = LEDC_TIMER_0;
static constexpr ledc_channel_t kChannelIn1 = LEDC_CHANNEL_0;
static constexpr ledc_channel_t kChannelIn2 = LEDC_CHANNEL_1;
static constexpr uint32_t kPwmFreqHz = 20000;
static constexpr ledc_timer_bit_t kPwmResolution = LEDC_TIMER_10_BIT;
static constexpr uint32_t kMaxDuty = (1U << 10U) - 1U;

float s_last_cmd = 0.0f;
uint32_t s_last_in1_duty = 0;
uint32_t s_last_in2_duty = 0;
int s_sleep_set_level = 0;
bool s_wake_failed = false;
bool s_awake = false;
int s_drive_sign = 0;
uint32_t s_kick_until_ms = 0;
// Allow ignoring sleep readback failures by default for this hardware setup
// so PWM output can be tested even if gpio readback of nSLEEP is unreliable.
bool s_ignore_wake = true;

static constexpr uint32_t kKickDurationMs = 40;
static constexpr float kKickTriggerCmd = 0.03f;

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

void internal_set_ignore_wake(bool ignore)
{
    s_ignore_wake = ignore;
    if (ignore) {
        // clear previous failure so motor_set will attempt to drive
        s_wake_failed = false;
    }
}

void set_channel(ledc_channel_t channel, uint32_t duty)
{
    ledc_set_duty(kLedcMode, channel, duty);
    ledc_update_duty(kLedcMode, channel);
    // Record last written duty for diagnostic reporting
    if (channel == kChannelIn1) {
        s_last_in1_duty = duty;
    } else if (channel == kChannelIn2) {
        s_last_in2_duty = duty;
    }
}

void set_sleep_level(int level)
{
    s_sleep_set_level = level ? 1 : 0;
    gpio_set_level(PIN_DRV_SLEEP, s_sleep_set_level);
}

// Try to wake the driver: set nSLEEP high and verify readback. If readback
// remains low, attempt a few toggles. If still failing, mark wake_failed.
static void try_wake_driver()
{
    if (s_awake && s_sleep_set_level == 1) {
        return;
    }

    s_wake_failed = false;
    set_sleep_level(1);
    // small delay to let levels settle
    vTaskDelay(pdMS_TO_TICKS(5));
    int read = gpio_get_level(PIN_DRV_SLEEP);
    if (read == 1) {
        s_wake_failed = false;
        s_awake = true;
        return;
    }

    // Try toggling a few times
    for (int i = 0; i < 3; ++i) {
        set_sleep_level(0);
        vTaskDelay(pdMS_TO_TICKS(5));
        set_sleep_level(1);
        vTaskDelay(pdMS_TO_TICKS(10));
        read = gpio_get_level(PIN_DRV_SLEEP);
        if (read == 1) {
            s_wake_failed = false;
            s_awake = true;
            return;
        }
    }

    s_wake_failed = true;
    s_awake = s_ignore_wake;
}

uint32_t cmd_to_duty(float cmd)
{
    const float duty = clampf(std::fabs(cmd), 0.0f, 1.0f) * static_cast<float>(kMaxDuty);
    return static_cast<uint32_t>(duty + 0.5f);
}

uint32_t millis()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

} // namespace

esp_err_t motor_init()
{
    gpio_config_t sleep_cfg {};
    sleep_cfg.pin_bit_mask = 1ULL << static_cast<uint32_t>(PIN_DRV_SLEEP);
    sleep_cfg.mode = GPIO_MODE_OUTPUT;
    sleep_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    sleep_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    sleep_cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&sleep_cfg));
    // Keep nSLEEP low during PWM setup.
    set_sleep_level(0);

    ledc_timer_config_t timer_cfg {};
    timer_cfg.speed_mode = kLedcMode;
    timer_cfg.duty_resolution = kPwmResolution;
    timer_cfg.timer_num = kLedcTimer;
    timer_cfg.freq_hz = kPwmFreqHz;
    timer_cfg.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    ledc_channel_config_t ch1 {};
    ch1.gpio_num = PIN_DRV_IN1;
    ch1.speed_mode = kLedcMode;
    ch1.channel = kChannelIn1;
    ch1.intr_type = LEDC_INTR_DISABLE;
    ch1.timer_sel = kLedcTimer;
    ch1.duty = 0;
    ch1.hpoint = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&ch1));

    ledc_channel_config_t ch2 {};
    ch2.gpio_num = PIN_DRV_IN2;
    ch2.speed_mode = kLedcMode;
    ch2.channel = kChannelIn2;
    ch2.intr_type = LEDC_INTR_DISABLE;
    ch2.timer_sel = kLedcTimer;
    ch2.duty = 0;
    ch2.hpoint = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&ch2));

    motor_sleep(true);
    return ESP_OK;
}

float motor_set(float cmd)
{
    cmd = clampf(cmd, -1.0f, 1.0f);
    if (std::fabs(cmd) < 0.001f) {
        motor_coast();
        return 0.0f;
    }
    const float control_cmd = cmd;

    // Command range is -1..1, so a limit of 2.0 is effectively no slew limit.
    // The balance loop needs immediate reversal when crossing the target angle.
    const float kMaxDelta = 2.0f; // no practical command slew limit
    if (std::fabs(cmd - s_last_cmd) > kMaxDelta) {
        if (cmd > s_last_cmd) cmd = s_last_cmd + kMaxDelta;
        else cmd = s_last_cmd - kMaxDelta;
    }
    // Apply minimum command deadzone compensation from persisted params
    {
        const ControlParams p = app_state_get_params();
        const int sign = (cmd > 0.0f) ? 1 : -1;
        const float gain = (sign > 0) ? p.gain_pos : p.gain_neg;
        cmd = clampf(cmd * clampf(gain, 0.0f, 3.0f), -1.0f, 1.0f);

        const float min_cmd = clampf((sign > 0) ? p.min_pos : p.min_neg, 0.0f, 1.0f);
        if (min_cmd > 0.0001f && std::fabs(cmd) >= 0.001f && std::fabs(cmd) < min_cmd) {
            cmd = (cmd > 0.0f) ? min_cmd : -min_cmd;
        }
        const uint32_t now = millis();
        if (sign != s_drive_sign) {
            s_kick_until_ms = (std::fabs(control_cmd) >= kKickTriggerCmd) ? now + kKickDurationMs : 0;
            s_drive_sign = sign;
        }
        const float kick_cmd = clampf((sign > 0) ? p.kick_pos : p.kick_neg, 0.0f, 1.0f);
        if (kick_cmd > 0.0001f && now < s_kick_until_ms && std::fabs(cmd) < kick_cmd) {
            cmd = (cmd > 0.0f) ? kick_cmd : -kick_cmd;
        }
        cmd = clampf(cmd, -p.output_limit, p.output_limit);
    }
    // Ensure driver is awake before driving PWM. This only blocks on the first
    // wake after sleep; normal control-loop updates return immediately.
    if (!s_awake || s_sleep_set_level == 0) {
        try_wake_driver();
    }
    if (s_wake_failed && !s_ignore_wake) {
        // If wake failed, avoid trying to drive the motor to prevent
        // unpredictable behavior; keep outputs coasting.
        motor_coast();
        s_last_cmd = 0.0f;
        return 0.0f;
    }

    set_sleep_level(1);
    s_awake = true;
    const uint32_t duty = cmd_to_duty(cmd);
    // Swap channels for reversed motor direction: positive cmd drives IN2
    if (cmd > 0.0f) {
        set_channel(kChannelIn1, 0);
        set_channel(kChannelIn2, duty);
        s_last_in1_duty = 0;
        s_last_in2_duty = duty;
    } else {
        set_channel(kChannelIn1, duty);
        set_channel(kChannelIn2, 0);
        s_last_in1_duty = duty;
        s_last_in2_duty = 0;
    }
    s_last_cmd = cmd;
    return cmd;
}

void motor_coast()
{
    set_channel(kChannelIn1, 0);
    set_channel(kChannelIn2, 0);
    s_last_cmd = 0.0f;
    s_last_in1_duty = 0;
    s_last_in2_duty = 0;
    s_drive_sign = 0;
    s_kick_until_ms = 0;
}

void motor_brake()
{
    set_sleep_level(1);
    set_channel(kChannelIn1, kMaxDuty);
    set_channel(kChannelIn2, kMaxDuty);
    s_last_cmd = 0.0f;
    s_last_in1_duty = kMaxDuty;
    s_last_in2_duty = kMaxDuty;
}

void motor_sleep(bool sleep)
{
    if (sleep) {
        motor_coast();
        set_sleep_level(0);
        s_awake = false;
    } else {
        try_wake_driver();
        set_sleep_level(1);
        s_awake = !s_wake_failed || s_ignore_wake;
    }
}

MotorDebug motor_debug_get()
{
    MotorDebug debug {};
    debug.cmd = s_last_cmd;
    // If recorded HW duty is zero but we have a last command, compute the
    // expected duty from the last command so diagnostics reflect the
    // controller's intent even when HW output is not readable.
    if (s_last_in1_duty == 0 && s_last_in2_duty == 0 && std::fabs(s_last_cmd) > 0.0001f) {
        const uint32_t expected = cmd_to_duty(s_last_cmd);
        if (s_last_cmd > 0.0f) {
            debug.in1_duty = 0;
            debug.in2_duty = expected;
        } else {
            debug.in1_duty = expected;
            debug.in2_duty = 0;
        }
    } else {
        debug.in1_duty = s_last_in1_duty;
        debug.in2_duty = s_last_in2_duty;
    }
    debug.max_duty = kMaxDuty;
    debug.sleep_set_level = s_sleep_set_level;
    debug.sleep_read_level = gpio_get_level(PIN_DRV_SLEEP);
    debug.wake_failed = s_wake_failed;
    return debug;
}

// External wrapper that calls the internal setter inside the anonymous
// namespace. This provides external linkage for the function declared in
// the header while keeping implementation details static.
void motor_force_ignore_wake(bool ignore)
{
    internal_set_ignore_wake(ignore);
}
