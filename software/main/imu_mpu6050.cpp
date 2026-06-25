#include "imu_mpu6050.h"

#include <cmath>
#include <cstdint>

#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

static const char *TAG = "mpu6050";
static constexpr i2c_port_t kI2cPort = I2C_NUM_0;
static constexpr uint32_t kI2cFreqHz = 400000;
static constexpr float kRadToDeg = 57.2957795f;
static constexpr float kComplementaryAlpha = 0.98f;
static constexpr float kGyroScale500Dps = 65.5f;
static constexpr float kAccelScale2g = 16384.0f;

// The MPU6050 module is mounted flat on the vertical PCB. On this GY-521 layout,
// X points roughly along the ladder height, Y points left/right on the PCB, and
// Z is normal to the PCB. Front/back tilt rotates around Y, so the angle comes
// from the X/Z gravity plane.
static constexpr int kDefaultAccelAngleAxis = 0;
static constexpr int kDefaultAccelReferenceAxis = 2;
static constexpr int kGyroControlAxis = 1;
static constexpr float kAccelAngleSign = 1.0f;
static constexpr float kGyroRateSign = 1.0f;
static constexpr float kBaseAngleOffsetDeg = 90.0f;
static constexpr float kDefaultMountOffsetDeg = 0.0f;

float s_gyro_bias_raw[3] = {0.0f, 0.0f, 0.0f};
int s_accel_angle_axis = kDefaultAccelAngleAxis;
int s_accel_reference_axis = kDefaultAccelReferenceAxis;
float s_mount_offset_deg = kDefaultMountOffsetDeg;
float s_angle_deg = 90.0f;
bool s_angle_valid = false;

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

float shortest_angle_error(float target_deg, float measured_deg)
{
    float error = wrap_360(target_deg) - wrap_360(measured_deg);
    if (error > 180.0f) {
        error -= 360.0f;
    } else if (error < -180.0f) {
        error += 360.0f;
    }
    return error;
}

int axis_index(char axis)
{
    switch (axis) {
    case 'x':
    case 'X':
        return 0;
    case 'y':
    case 'Y':
        return 1;
    case 'z':
    case 'Z':
        return 2;
    default:
        return -1;
    }
}

char axis_name(int axis)
{
    static constexpr char kNames[] = {'x', 'y', 'z'};
    if (axis < 0 || axis > 2) {
        return '?';
    }
    return kNames[axis];
}

esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    return i2c_master_write_to_device(kI2cPort, MPU6050_I2C_ADDR, data, sizeof(data), pdMS_TO_TICKS(100));
}

esp_err_t read_regs(uint8_t start_reg, uint8_t *data, size_t len)
{
    return i2c_master_write_read_device(kI2cPort, MPU6050_I2C_ADDR, &start_reg, 1, data, len, pdMS_TO_TICKS(100));
}

esp_err_t probe_addr(uint8_t addr)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, static_cast<uint8_t>((addr << 1U) | I2C_MASTER_WRITE), true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(kI2cPort, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    return err;
}

void scan_i2c_bus()
{
    int found = 0;
    ESP_LOGI(TAG, "scan I2C bus SDA=%d SCL=%d freq=%lu", PIN_MPU_SDA, PIN_MPU_SCL, static_cast<unsigned long>(kI2cFreqHz));
    for (uint8_t addr = 0x08; addr <= 0x77; ++addr) {
        if (probe_addr(addr) == ESP_OK) {
            ESP_LOGI(TAG, "I2C device ACK at 0x%02X", addr);
            ++found;
        }
    }

    if (found == 0) {
        ESP_LOGW(TAG, "I2C scan found no devices on MPU bus");
    }
}

int16_t be16(const uint8_t *data)
{
    return static_cast<int16_t>((static_cast<uint16_t>(data[0]) << 8U) | data[1]);
}

esp_err_t read_raw(int16_t accel[3], int16_t gyro[3])
{
    uint8_t data[14] {};
    esp_err_t err = read_regs(0x3B, data, sizeof(data));
    if (err != ESP_OK) {
        return err;
    }

    accel[0] = be16(&data[0]);
    accel[1] = be16(&data[2]);
    accel[2] = be16(&data[4]);
    gyro[0] = be16(&data[8]);
    gyro[1] = be16(&data[10]);
    gyro[2] = be16(&data[12]);
    return ESP_OK;
}

float accel_angle_deg(const int16_t accel[3])
{
    float a[3] = {
        static_cast<float>(accel[0]) / kAccelScale2g,
        static_cast<float>(accel[1]) / kAccelScale2g,
        static_cast<float>(accel[2]) / kAccelScale2g,
    };
    return wrap_360(kBaseAngleOffsetDeg + s_mount_offset_deg +
                    kAccelAngleSign * std::atan2(a[s_accel_angle_axis], a[s_accel_reference_axis]) * kRadToDeg);
}

float gyro_rate_deg_s(const int16_t gyro[3])
{
    const float corrected = static_cast<float>(gyro[kGyroControlAxis]) - s_gyro_bias_raw[kGyroControlAxis];
    return kGyroRateSign * corrected / kGyroScale500Dps;
}

} // namespace

esp_err_t imu_init()
{
    i2c_config_t cfg {};
    cfg.mode = I2C_MODE_MASTER;
    cfg.sda_io_num = PIN_MPU_SDA;
    cfg.scl_io_num = PIN_MPU_SCL;
    cfg.sda_pullup_en = GPIO_PULLUP_ENABLE;
    cfg.scl_pullup_en = GPIO_PULLUP_ENABLE;
    cfg.master.clk_speed = kI2cFreqHz;

    ESP_LOGI(TAG, "init MPU6050 bus SDA=%d SCL=%d addr=0x%02X", PIN_MPU_SDA, PIN_MPU_SCL, MPU6050_I2C_ADDR);
    ESP_ERROR_CHECK(i2c_param_config(kI2cPort, &cfg));
    esp_err_t err = i2c_driver_install(kI2cPort, cfg.mode, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "i2c_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
    scan_i2c_bus();

    uint8_t who = 0;
    err = read_regs(0x75, &who, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WHO_AM_I read failed at 0x%02X: %s", MPU6050_I2C_ADDR, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "WHO_AM_I=0x%02X", who);
    if (who != 0x68) {
        ESP_LOGW(TAG, "unexpected WHO_AM_I=0x%02X, expected 0x68 for MPU6050", who);
    }

    err = write_reg(0x6B, 0x00); // PWR_MGMT_1
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write PWR_MGMT_1 failed: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    err = write_reg(0x19, 0x04); // SMPLRT_DIV, 200 Hz class data rate with DLPF
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write SMPLRT_DIV failed: %s", esp_err_to_name(err));
        return err;
    }
    err = write_reg(0x1A, 0x03); // CONFIG, DLPF
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write CONFIG failed: %s", esp_err_to_name(err));
        return err;
    }
    err = write_reg(0x1B, 0x08); // GYRO_CONFIG, +/-500 dps
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write GYRO_CONFIG failed: %s", esp_err_to_name(err));
        return err;
    }
    err = write_reg(0x1C, 0x00); // ACCEL_CONFIG, +/-2 g
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write ACCEL_CONFIG failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

esp_err_t imu_calibrate()
{
    constexpr int kSamples = 400;
    int64_t gyro_sum[3] = {0, 0, 0};
    int16_t accel[3] {};
    int16_t gyro[3] {};

    for (int i = 0; i < kSamples; ++i) {
        esp_err_t err = read_raw(accel, gyro);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "calibration read_raw failed at sample %d/%d: %s", i + 1, kSamples, esp_err_to_name(err));
            return err;
        }
        gyro_sum[0] += gyro[0];
        gyro_sum[1] += gyro[1];
        gyro_sum[2] += gyro[2];
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    s_gyro_bias_raw[0] = static_cast<float>(gyro_sum[0]) / kSamples;
    s_gyro_bias_raw[1] = static_cast<float>(gyro_sum[1]) / kSamples;
    s_gyro_bias_raw[2] = static_cast<float>(gyro_sum[2]) / kSamples;
    s_angle_deg = accel_angle_deg(accel);
    s_angle_valid = true;
    ESP_LOGI(TAG, "gyro bias raw x/y/z: %.1f %.1f %.1f, accel raw x/y/z: %d %d %d, initial angle %.2f",
             s_gyro_bias_raw[0], s_gyro_bias_raw[1], s_gyro_bias_raw[2],
             accel[0], accel[1], accel[2], s_angle_deg);
    return ESP_OK;
}

bool imu_set_accel_axes(char angle_axis, char reference_axis)
{
    const int angle = axis_index(angle_axis);
    const int reference = axis_index(reference_axis);
    if (angle < 0 || reference < 0 || angle == reference) {
        return false;
    }

    s_accel_angle_axis = angle;
    s_accel_reference_axis = reference;
    s_angle_valid = false;
    ESP_LOGI(TAG, "accel angle axes set to atan2(%c,%c)", axis_name(s_accel_angle_axis), axis_name(s_accel_reference_axis));
    return true;
}

void imu_get_accel_axes(char *angle_axis, char *reference_axis)
{
    if (angle_axis != nullptr) {
        *angle_axis = axis_name(s_accel_angle_axis);
    }
    if (reference_axis != nullptr) {
        *reference_axis = axis_name(s_accel_reference_axis);
    }
}

esp_err_t imu_set_current_angle(float desired_angle_deg)
{
    int16_t accel[3] {};
    int16_t gyro[3] {};
    esp_err_t err = read_raw(accel, gyro);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "level read_raw failed: %s", esp_err_to_name(err));
        return err;
    }

    const float old_angle = accel_angle_deg(accel);
    const float error = shortest_angle_error(desired_angle_deg, old_angle);
    s_mount_offset_deg = wrap_360(s_mount_offset_deg + error);
    s_angle_deg = wrap_360(desired_angle_deg);
    s_angle_valid = true;

    ESP_LOGI(TAG, "level angle old=%.2f desired=%.2f mount_offset=%.2f accel raw x/y/z=%d %d %d",
             old_angle, s_angle_deg, s_mount_offset_deg, accel[0], accel[1], accel[2]);
    return ESP_OK;
}

esp_err_t imu_update(float dt_s, ImuReading *reading)
{
    if (reading == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    int16_t accel[3] {};
    int16_t gyro[3] {};
    esp_err_t err = read_raw(accel, gyro);
    if (err != ESP_OK) {
        reading->ok = false;
        return err;
    }

    // Compute raw accelerometer plane angle (atan2 of accel axes) without
    // applying the base 90-degree offset or the mount_offset. This is the
    // direct plane angle you select and want printed to choose the upright
    // angle.
    const float raw_plane = kAccelAngleSign * std::atan2(
        static_cast<float>(accel[s_accel_angle_axis]) / kAccelScale2g,
        static_cast<float>(accel[s_accel_reference_axis]) / kAccelScale2g) * kRadToDeg;
    const float acc_angle = wrap_360(kBaseAngleOffsetDeg + s_mount_offset_deg + raw_plane);
    const float rate = gyro_rate_deg_s(gyro);
    if (!s_angle_valid) {
        s_angle_deg = acc_angle;
        s_angle_valid = true;
    } else {
        const float predicted = wrap_360(s_angle_deg + rate * dt_s);
        s_angle_deg = wrap_360(predicted + (1.0f - kComplementaryAlpha) * shortest_angle_error(acc_angle, predicted));
    }

    // Expose raw plane and accel-based adjusted angle for diagnostics.
    reading->accel_plane_deg = wrap_360(raw_plane);
    reading->accel_angle_deg = acc_angle;
    // Preserve the fused/complementary `s_angle_deg` as the primary
    // `angle_deg` value for callers that rely on a filtered attitude.
    reading->angle_deg = s_angle_deg;
    reading->gyro_rate_deg_s = rate;
    reading->ok = true;
    return ESP_OK;
}
