#pragma once

#include <cstddef>

#include "app_state.h"
#include "esp_err.h"

using BleCommandCallback = void (*)(const char *command, char *response, size_t response_len);

esp_err_t ble_debug_init(BleCommandCallback callback);
bool ble_debug_is_connected();
void ble_debug_send_line(const char *line);
void ble_debug_notify_telemetry(const Telemetry &telemetry);
