#include "balance_controller.h"

#include <cmath>

namespace {

float s_integral = 0.0f;
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

} // namespace

void balance_controller_reset()
{
    s_integral = 0.0f;
}

float balance_controller_update(float angle_deg, float gyro_rate_deg_s, const ControlParams &params, float dt_s)
{
    const float error = app_angle_error_deg(params.target_angle_deg, angle_deg);
    // Target is normally constant, so error derivative is approximately
    // -angle_rate. Using gyro directly gives a faster D term than differencing
    // the filtered angle.
    const float d_error = std::isfinite(gyro_rate_deg_s) ? -gyro_rate_deg_s : 0.0f;

    // Non-linear proportional boost for small errors: amplify P term when
    // abs(error) is small so that small deviations produce stronger corrective
    // motor commands. The exponential term decays as error grows.
    const float boost = 1.0f + 2.0f * std::exp(-std::fabs(error) / 10.0f);
    const float kp_eff = params.kp * boost;

    const float candidate_integral = s_integral + error * dt_s;
    const float unsat = kp_eff * error + params.ki * candidate_integral + params.kd * d_error;
    const float limited = clampf(unsat, -params.output_limit, params.output_limit);

    const bool saturated = std::fabs(unsat - limited) > 0.0001f;
    const bool saturation_getting_worse = saturated && ((unsat > 0.0f && error > 0.0f) || (unsat < 0.0f && error < 0.0f));
    if (!saturation_getting_worse) {
        s_integral = clampf(candidate_integral, -200.0f, 200.0f);
    }

    return limited;
}
