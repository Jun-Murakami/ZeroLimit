#include "Limiter.h"
#include <cmath>
#include <algorithm>

namespace zl::dsp {

void ZeroLatencyLimiter::prepare(double sampleRate, int /*numChannels*/)
{
    currentSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    updateReleaseCoeff();
    reset();
}

void ZeroLatencyLimiter::reset()
{
    currentGain = 1.0f;
}

void ZeroLatencyLimiter::setThresholdDb(float thresholdDb)
{
    // -80dB で実質オフ相当の下限を設定。上限は 0dB
    const float clamped = std::max(-80.0f, std::min(0.0f, thresholdDb));
    thresholdLin = std::pow(10.0f, clamped / 20.0f);
}

void ZeroLatencyLimiter::setReleaseMs(float ms)
{
    releaseMs = std::max(1.0f, std::min(2000.0f, ms));
    updateReleaseCoeff();
}

void ZeroLatencyLimiter::updateReleaseCoeff()
{
    // 1 サンプル当たりの乗算係数。targetGain へ exp で近づく。
    // coeff = exp(-1 / (tau_samples)) （tau_samples = release_sec * fs）
    const double tauSamples = (static_cast<double>(releaseMs) * 0.001) * currentSampleRate;
    releaseCoeff = tauSamples > 0.0 ? static_cast<float>(std::exp(-1.0 / tauSamples)) : 0.0f;
}

float ZeroLatencyLimiter::processSample(float& sampleL, float& sampleR) noexcept
{
    const float absMax = std::max(std::abs(sampleL), std::abs(sampleR));

    // 目標ゲイン: |x| が threshold を超えるときのみ threshold/|x| に落とす
    float targetGain = 1.0f;
    if (absMax > thresholdLin && absMax > 0.0f)
        targetGain = thresholdLin / absMax;

    // アタック即時（下方向）、リリースは時定数で上方向へ緩やかに
    if (targetGain < currentGain)
        currentGain = targetGain;
    else
        currentGain = targetGain + (currentGain - targetGain) * releaseCoeff;

    sampleL *= currentGain;
    sampleR *= currentGain;
    return currentGain;
}

float ZeroLatencyLimiter::processBlock(juce::AudioBuffer<float>& buffer) noexcept
{
    const int numChannels = buffer.getNumChannels();
    const int numSamples  = buffer.getNumSamples();
    if (numChannels <= 0 || numSamples <= 0)
        return 1.0f;

    float minGainObserved = 1.0f;

    // モノラル時も同じロジックで問題なく動作するよう、absMax は可変チャネルで算出
    if (numChannels == 1)
    {
        auto* ch = buffer.getWritePointer(0);
        for (int i = 0; i < numSamples; ++i)
        {
            const float a = std::abs(ch[i]);
            float targetGain = 1.0f;
            if (a > thresholdLin && a > 0.0f)
                targetGain = thresholdLin / a;

            if (targetGain < currentGain) currentGain = targetGain;
            else                          currentGain = targetGain + (currentGain - targetGain) * releaseCoeff;

            ch[i] *= currentGain;
            if (currentGain < minGainObserved) minGainObserved = currentGain;
        }
        return minGainObserved;
    }

    auto* left  = buffer.getWritePointer(0);
    auto* right = buffer.getWritePointer(std::min(1, numChannels - 1));

    for (int i = 0; i < numSamples; ++i)
    {
        const float a = std::max(std::abs(left[i]), std::abs(right[i]));

        float targetGain = 1.0f;
        if (a > thresholdLin && a > 0.0f)
            targetGain = thresholdLin / a;

        if (targetGain < currentGain) currentGain = targetGain;
        else                          currentGain = targetGain + (currentGain - targetGain) * releaseCoeff;

        left[i]  *= currentGain;
        right[i] *= currentGain;

        // 追加チャネル（>2ch）は左右ペアに倣ってゲインを掛ける
        for (int ch = 2; ch < numChannels; ++ch)
            buffer.getWritePointer(ch)[i] *= currentGain;

        if (currentGain < minGainObserved) minGainObserved = currentGain;
    }
    return minGainObserved;
}

} // namespace zl::dsp
