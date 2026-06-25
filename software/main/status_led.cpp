#include "status_led.h"

#include "board_pins.h"
#include "driver/gpio.h"

esp_err_t status_led_init()
{
    gpio_config_t cfg {};
    cfg.pin_bit_mask = 1ULL << static_cast<uint32_t>(PIN_STATUS_LED);
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    esp_err_t err = gpio_config(&cfg);
    status_led_set(false);
    return err;
}

void status_led_set(bool on)
{
    const int level = STATUS_LED_ACTIVE_LOW ? (on ? 0 : 1) : (on ? 1 : 0);
    gpio_set_level(PIN_STATUS_LED, level);
}

void status_led_update(AppState state, uint32_t now_ms)
{
    bool on = false;

    switch (state) {
    case AppState::BOOT:
    case AppState::CALIBRATING:
        on = (now_ms % 500U) < 250U;
        break;
    case AppState::DISARMED:
        on = (now_ms % 2000U) < 1000U;
        break;
    case AppState::READY:
        on = (now_ms % 250U) < 125U;
        break;
    case AppState::ARMED:
        on = (now_ms % 1000U) >= 50U;
        break;
    case AppState::FAULT:
        on = (now_ms % 100U) < 50U;
        break;
    default:
        on = false;
        break;
    }

    status_led_set(on);
}
