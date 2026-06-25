#pragma once

#include "app_state.h"
#include "esp_err.h"

esp_err_t ui_oled_init();
void ui_oled_update(const Telemetry &telemetry);
