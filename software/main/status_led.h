#pragma once

#include <cstdint>

#include "app_state.h"
#include "esp_err.h"

esp_err_t status_led_init();
void status_led_set(bool on);
void status_led_update(AppState state, uint32_t now_ms);
