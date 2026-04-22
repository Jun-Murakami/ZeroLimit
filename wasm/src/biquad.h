// Direct Form II Transposed biquad (single channel state).
// Coefficients follow the RBJ cookbook convention:
//   y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
// with a0 normalised to 1.
#pragma once
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace zl_wasm {

struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void reset() noexcept { z1 = z2 = 0.0f; }

    inline float process(float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    // RBJ cookbook 2nd-order Butterworth LP (a0 normalised).
    static void makeLowPass(Biquad& bq, double sampleRate, double fc, double Q)
    {
        const double w0 = 2.0 * M_PI * fc / sampleRate;
        const double cw = std::cos(w0);
        const double sw = std::sin(w0);
        const double alpha = sw / (2.0 * Q);

        const double a0 = 1.0 + alpha;
        bq.b0 = static_cast<float>((1.0 - cw) * 0.5 / a0);
        bq.b1 = static_cast<float>((1.0 - cw) / a0);
        bq.b2 = static_cast<float>((1.0 - cw) * 0.5 / a0);
        bq.a1 = static_cast<float>(-2.0 * cw / a0);
        bq.a2 = static_cast<float>((1.0 - alpha) / a0);
    }

    static void makeHighPass(Biquad& bq, double sampleRate, double fc, double Q)
    {
        const double w0 = 2.0 * M_PI * fc / sampleRate;
        const double cw = std::cos(w0);
        const double sw = std::sin(w0);
        const double alpha = sw / (2.0 * Q);

        const double a0 = 1.0 + alpha;
        bq.b0 = static_cast<float>((1.0 + cw) * 0.5 / a0);
        bq.b1 = static_cast<float>(-(1.0 + cw) / a0);
        bq.b2 = static_cast<float>((1.0 + cw) * 0.5 / a0);
        bq.a1 = static_cast<float>(-2.0 * cw / a0);
        bq.a2 = static_cast<float>((1.0 - alpha) / a0);
    }
};

} // namespace zl_wasm
