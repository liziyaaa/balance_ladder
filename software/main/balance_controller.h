#pragma once

#include "app_state.h"

void balance_controller_reset();
float balance_controller_update(float angle_deg, const ControlParams &params, float dt_s);
