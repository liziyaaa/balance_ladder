#include "ui_oled.h"

#include <cmath>
#include <cstring>

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

namespace {

static constexpr int kWidth = 128;
static constexpr int kHeight = 64;
static constexpr int kPages = kHeight / 8;
static constexpr int kFrameSize = kWidth * kPages;
static const char *TAG = "ui_oled";
static uint8_t s_fb[kFrameSize] {};
static bool s_ok = false;
static uint32_t s_frame = 0;
static uint32_t s_stable_since_ms = 0;

uint32_t millis()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

void delay_us(uint32_t us)
{
    esp_rom_delay_us(us);
}

void sda(bool level)
{
    gpio_set_level(PIN_OLED_SDA, level ? 1 : 0);
}

void scl(bool level)
{
    gpio_set_level(PIN_OLED_SCL, level ? 1 : 0);
}

void i2c_start()
{
    sda(true);
    scl(true);
    delay_us(4);
    sda(false);
    delay_us(4);
    scl(false);
}

void i2c_stop()
{
    sda(false);
    scl(true);
    delay_us(4);
    sda(true);
    delay_us(4);
}

bool i2c_write_byte(uint8_t value)
{
    for (int i = 0; i < 8; ++i) {
        sda((value & 0x80U) != 0);
        delay_us(2);
        scl(true);
        delay_us(4);
        scl(false);
        value <<= 1U;
    }

    sda(true);
    delay_us(2);
    scl(true);
    delay_us(4);
    const bool ack = gpio_get_level(PIN_OLED_SDA) == 0;
    scl(false);
    return ack;
}

bool oled_command(uint8_t cmd)
{
    i2c_start();
    const bool ok = i2c_write_byte(static_cast<uint8_t>(OLED_I2C_ADDR << 1U)) &&
                    i2c_write_byte(0x00) &&
                    i2c_write_byte(cmd);
    i2c_stop();
    return ok;
}

bool oled_data(const uint8_t *data, size_t len)
{
    i2c_start();
    bool ok = i2c_write_byte(static_cast<uint8_t>(OLED_I2C_ADDR << 1U)) &&
              i2c_write_byte(0x40);
    for (size_t i = 0; i < len; ++i) {
        ok = i2c_write_byte(data[i]) && ok;
    }
    i2c_stop();
    return ok;
}

void clear()
{
    std::memset(s_fb, 0, sizeof(s_fb));
}

void pixel(int x, int y, bool on = true)
{
    if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) {
        return;
    }
    const int index = x + (y / 8) * kWidth;
    const uint8_t mask = static_cast<uint8_t>(1U << (y & 7));
    if (on) {
        s_fb[index] |= mask;
    } else {
        s_fb[index] &= static_cast<uint8_t>(~mask);
    }
}

void fill_rect(int x, int y, int w, int h, bool on = true)
{
    for (int yy = y; yy < y + h; ++yy) {
        for (int xx = x; xx < x + w; ++xx) {
            pixel(xx, yy, on);
        }
    }
}

void line(int x0, int y0, int x1, int y1)
{
    int dx = std::abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        pixel(x0, y0);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void circle(int cx, int cy, int r)
{
    int x = -r;
    int y = 0;
    int err = 2 - 2 * r;
    do {
        pixel(cx - x, cy + y);
        pixel(cx - y, cy - x);
        pixel(cx + x, cy - y);
        pixel(cx + y, cy + x);
        const int e2 = err;
        if (e2 <= y) {
            err += ++y * 2 + 1;
        }
        if (e2 > x || err > y) {
            err += ++x * 2 + 1;
        }
    } while (x < 0);
}

void filled_circle(int cx, int cy, int r)
{
    for (int y = -r; y <= r; ++y) {
        for (int x = -r; x <= r; ++x) {
            if (x * x + y * y <= r * r) {
                pixel(cx + x, cy + y);
            }
        }
    }
}

void draw_face_base()
{
    circle(64, 32, 28);
    circle(64, 32, 27);
}

void draw_waiting(bool frame)
{
    draw_face_base();
    fill_rect(49, 24, 8, 3);
    fill_rect(72, 24, 8, 3);
    if (frame) {
        line(49, 44, 79, 44);
    } else {
        line(52, 43, 76, 45);
    }
}

void draw_happy(bool frame)
{
    draw_face_base();
    filled_circle(52, 25, 3);
    filled_circle(76, 25, 3);
    line(48, 41, 55, 48);
    line(55, 48, 64, 50);
    line(64, 50, 73, 48);
    line(73, 48, 80, 41);
    if (frame) {
        line(44, 16, 54, 13);
        line(74, 13, 84, 16);
    }
}

void draw_effort(bool frame)
{
    draw_face_base();
    line(45, frame ? 20 : 18, 58, 25);
    line(83, frame ? 20 : 18, 70, 25);
    fill_rect(50, 29, 6, 3);
    fill_rect(72, 29, 6, 3);
    line(52, 47, 76, 43);
    line(52, 48, 76, 44);
}

void draw_sad(bool frame)
{
    draw_face_base();
    fill_rect(49, 25, 7, 3);
    fill_rect(73, 25, 7, 3);
    line(48, 49, 56, 43);
    line(56, 43, 64, 41);
    line(64, 41, 72, 43);
    line(72, 43, 80, 49);
    if (frame) {
        line(84, 31, 88, 38);
        line(88, 38, 86, 43);
    }
}

void display()
{
    for (uint8_t page = 0; page < kPages; ++page) {
        oled_command(static_cast<uint8_t>(0xB0 | page));
        oled_command(0x00);
        oled_command(0x10);
        oled_data(&s_fb[page * kWidth], kWidth);
    }
}

} // namespace

esp_err_t ui_oled_init()
{
    gpio_config_t cfg {};
    cfg.pin_bit_mask = (1ULL << static_cast<uint32_t>(PIN_OLED_SDA)) |
                       (1ULL << static_cast<uint32_t>(PIN_OLED_SCL));
    cfg.mode = GPIO_MODE_OUTPUT_OD;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&cfg));
    sda(true);
    scl(true);
    delay_us(50000);

    const uint8_t init_cmds[] = {
        0xAE,
        0xD5, 0x80,
        0xA8, 0x3F,
        0xD3, 0x00,
        0x40,
        0x8D, 0x14,
        0x20, 0x02, // Page addressing mode, matching display().
        0xA1,
        0xC8,
        0xDA, 0x12,
        0x81, 0xCF,
        0xD9, 0xF1,
        0xDB, 0x40,
        0xA4,
        0xA6,
        0xAF,
    };

    for (uint8_t cmd : init_cmds) {
        if (!oled_command(cmd)) {
            ESP_LOGW(TAG, "OLED 0x%02X no ACK on GPIO SDA=%d SCL=%d", OLED_I2C_ADDR, PIN_OLED_SDA, PIN_OLED_SCL);
            s_ok = false;
            return ESP_FAIL;
        }
    }

    clear();
    display();
    s_ok = true;
    ESP_LOGI(TAG, "OLED init OK at 0x%02X on SDA=%d SCL=%d", OLED_I2C_ADDR, PIN_OLED_SDA, PIN_OLED_SCL);
    return ESP_OK;
}

void ui_oled_update(const Telemetry &telemetry)
{
    if (!s_ok) {
        return;
    }

    clear();
    const bool frame = (s_frame++ & 1U) != 0;
    if (telemetry.state == AppState::FAULT) {
        s_stable_since_ms = 0;
        draw_sad(frame);
    } else if (telemetry.state == AppState::ARMED) {
        const uint32_t now = millis();
        if (std::fabs(telemetry.error_deg) < 2.0f) {
            if (s_stable_since_ms == 0) {
                s_stable_since_ms = now;
            }
        } else {
            s_stable_since_ms = 0;
        }

        if (s_stable_since_ms != 0 && now - s_stable_since_ms >= 1000U) {
            draw_happy(frame);
        } else {
            draw_effort(frame);
        }
    } else {
        s_stable_since_ms = 0;
        draw_waiting(frame);
    }
    display();
}
