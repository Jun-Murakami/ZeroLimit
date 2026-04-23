#include "Limiter.h"
#include <cmath>
#include <algorithm>

namespace zl::dsp {

namespace {
    inline float sanitizeFinite(float value) noexcept
    {
        return std::isfinite(value) ? value : 0.0f;
    }
}

void ZeroLatencyLimiter::prepare(double sampleRate, int /*numChannels*/)
{
    currentSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    updateReleaseCoeffs();
    reset();
}

void ZeroLatencyLimiter::reset()
{
    currentGain     = 1.0f;
    currentGainSlow = 1.0f;
}

void ZeroLatencyLimiter::setThresholdDb(float thresholdDb)
{
    // -80dB で実質オフ相当の下限を設定。上限は 0dB
    const float clamped = std::max(-80.0f, std::min(0.0f, thresholdDb));
    thresholdLin = std::pow(10.0f, clamped / 20.0f);
}

void ZeroLatencyLimiter::setReleaseMs(float ms)
{
    // 0.01ms .. 1000ms の範囲を許容。Auto Release モードでは fast envelope として使われる。
    releaseMs = std::max(0.01f, std::min(1000.0f, ms));
    updateReleaseCoeffs();
}

void ZeroLatencyLimiter::setSlowReleaseMs(float ms)
{
    // Slow envelope の時定数。マルチバンドで低域を遅めに・高域を速めにするのに使う。
    autoSlowReleaseMs = std::max(1.0f, std::min(2000.0f, ms));
    updateReleaseCoeffs();
}

void ZeroLatencyLimiter::updateReleaseCoeffs()
{
    // coeff = exp(-1 / tau_samples)。tau_samples = (ms * 1e-3) * fs
    const double tauFast = (static_cast<double>(releaseMs)             * 0.001) * currentSampleRate;
    const double tauSlow = (static_cast<double>(autoSlowReleaseMs)     * 0.001) * currentSampleRate;
    releaseCoeff     = tauFast > 0.0 ? static_cast<float>(std::exp(-1.0 / tauFast)) : 0.0f;
    slowReleaseCoeff = tauSlow > 0.0 ? static_cast<float>(std::exp(-1.0 / tauSlow)) : 0.0f;
}

float ZeroLatencyLimiter::processSample(float& sampleL, float& sampleR) noexcept
{
    sampleL = sanitizeFinite(sampleL);
    sampleR = sanitizeFinite(sampleR);

    const float absMax = std::max(std::abs(sampleL), std::abs(sampleR));

    // 目標ゲイン: |x| が threshold を超えるときのみ threshold/|x| に落とす
    float targetGain = 1.0f;
    if (absMax > thresholdLin && absMax > 0.0f)
        targetGain = thresholdLin / absMax;

    // fast envelope（アタック瞬時、指数リリース）
    if (targetGain < currentGain) currentGain = targetGain;
    else                          currentGain = targetGain + (currentGain - targetGain) * releaseCoeff;

    // slow envelope（常時更新して Auto Release 切替時のグリッチを避ける）
    if (targetGain < currentGainSlow) currentGainSlow = targetGain;
    else                              currentGainSlow = targetGain + (currentGainSlow - targetGain) * slowReleaseCoeff;

    const float applied = autoReleaseEnabled ? std::min(currentGain, currentGainSlow) : currentGain;

    sampleL *= applied;
    sampleR *= applied;
    return applied;
}

float ZeroLatencyLimiter::processBlock(juce::AudioBuffer<float>& buffer) noexcept
{
    const int numChannels = buffer.getNumChannels();
    const int numSamples  = buffer.getNumSamples();
    if (numChannels <= 0 || numSamples <= 0)
        return 1.0f;

    float minGainObserved = 1.0f;

    const bool arc = autoReleaseEnabled;

    if (numChannels == 1)
    {
        auto* ch = buffer.getWritePointer(0);
        for (int i = 0; i < numSamples; ++i)
        {
            ch[i] = sanitizeFinite(ch[i]);
            const float a = std::abs(ch[i]);
            float targetGain = 1.0f;
            if (a > thresholdLin && a > 0.0f)
                targetGain = thresholdLin / a;

            if (targetGain < currentGain) currentGain = targetGain;
            else                          currentGain = targetGain + (currentGain - targetGain) * releaseCoeff;

            if (targetGain < currentGainSlow) currentGainSlow = targetGain;
            else                              currentGainSlow = targetGain + (currentGainSlow - targetGain) * slowReleaseCoeff;

            const float applied = arc ? std::min(currentGain, currentGainSlow) : currentGain;

            ch[i] *= applied;
            if (applied < minGainObserved) minGainObserved = applied;
        }
        return minGainObserved;
    }

    auto* left  = buffer.getWritePointer(0);
    auto* right = buffer.getWritePointer(std::min(1, numChannels - 1));

    for (int i = 0; i < numSamples; ++i)
    {
        left[i]  = sanitizeFinite(left[i]);
        right[i] = sanitizeFinite(right[i]);

        const float a = std::max(std::abs(left[i]), std::abs(right[i]));

        float targetGain = 1.0f;
        if (a > thresholdLin && a > 0.0f)
            targetGain = thresholdLin / a;

        if (targetGain < currentGain) currentGain = targetGain;
        else                          currentGain = targetGain + (currentGain - targetGain) * releaseCoeff;

        if (targetGain < currentGainSlow) currentGainSlow = targetGain;
        else                              currentGainSlow = targetGain + (currentGainSlow - targetGain) * slowReleaseCoeff;

        const float applied = arc ? std::min(currentGain, currentGainSlow) : currentGain;

        left[i]  *= applied;
        right[i] *= applied;

        for (int ch = 2; ch < numChannels; ++ch)
        {
            auto* extra = buffer.getWritePointer(ch);
            extra[i] = sanitizeFinite(extra[i]) * applied;
        }

        if (applied < minGainObserved) minGainObserved = applied;
    }
    return minGainObserved;
}

} // namespace zl::dsp
