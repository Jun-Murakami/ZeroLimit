// JUCE を使わないゼロレイテンシー・ブリックウォール・リミッター（WASM 用）。
// プラグイン側の ZeroLatencyLimiter (plugin/src/dsp/Limiter.*) と同じ音の挙動を持つ。
#pragma once

#include <algorithm>
#include <cmath>

namespace zl_wasm {

class ZeroLatencyLimiter
{
public:
    void prepare(double sr) noexcept
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        updateCoeffs();
        reset();
    }

    void reset() noexcept { currentGain = currentGainSlow = 1.0f; }

    void setThresholdDb(float db) noexcept
    {
        const float c = std::max(-80.0f, std::min(0.0f, db));
        thresholdLin = std::pow(10.0f, c / 20.0f);
    }

    // Manual release / Auto Release fast envelope time constant
    void setReleaseMs(float ms) noexcept
    {
        releaseMs = std::max(0.01f, std::min(1000.0f, ms));
        updateCoeffs();
    }

    // Auto Release slow envelope time constant（バンド別の設定に使う）
    void setSlowReleaseMs(float ms) noexcept
    {
        slowReleaseMs = std::max(1.0f, std::min(2000.0f, ms));
        updateCoeffs();
    }

    void setAutoReleaseEnabled(bool e) noexcept { autoReleaseEnabled = e; }

    // ステレオ in-place 処理。戻り値は区間中の最小ゲイン（= 最大リダクション）。
    //  gainOut != nullptr なら各サンプルで適用した gain（リニア 0..1）を書き出す。
    //  配列長は最低でも numSamples 必要。
    float processStereoInPlace(float* L, float* R, int numSamples, float* gainOut = nullptr) noexcept
    {
        float minG = 1.0f;
        const bool arc = autoReleaseEnabled;
        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = sanitizeFinite(L[i]);
            const float inR = sanitizeFinite(R[i]);
            const float a = std::max(std::abs(inL), std::abs(inR));
            float target = 1.0f;
            if (a > thresholdLin && a > 0.0f)
                target = thresholdLin / a;

            if (target < currentGain) currentGain = target;
            else                      currentGain = target + (currentGain - target) * releaseCoeff;

            if (target < currentGainSlow) currentGainSlow = target;
            else                          currentGainSlow = target + (currentGainSlow - target) * slowReleaseCoeff;

            const float applied = arc ? std::min(currentGain, currentGainSlow) : currentGain;
            L[i] = inL * applied;
            R[i] = inR * applied;
            if (gainOut) gainOut[i] = applied;
            if (applied < minG) minG = applied;
        }
        return minG;
    }

private:
    static float sanitizeFinite(float v) noexcept
    {
        return std::isfinite(v) ? v : 0.0f;
    }

    void updateCoeffs() noexcept
    {
        const double tauFast = static_cast<double>(releaseMs)     * 0.001 * sampleRate;
        const double tauSlow = static_cast<double>(slowReleaseMs) * 0.001 * sampleRate;
        releaseCoeff     = tauFast > 0.0 ? static_cast<float>(std::exp(-1.0 / tauFast)) : 0.0f;
        slowReleaseCoeff = tauSlow > 0.0 ? static_cast<float>(std::exp(-1.0 / tauSlow)) : 0.0f;
    }

    double sampleRate      = 44100.0;
    float  thresholdLin    = 1.0f;
    float  releaseMs       = 1.0f;
    float  slowReleaseMs   = 150.0f;
    float  releaseCoeff    = 0.9995f;
    float  slowReleaseCoeff= 0.9999f;
    float  currentGain     = 1.0f;
    float  currentGainSlow = 1.0f;
    bool   autoReleaseEnabled = true;
};

} // namespace zl_wasm
