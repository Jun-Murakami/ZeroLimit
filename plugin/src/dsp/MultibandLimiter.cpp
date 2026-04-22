#include "MultibandLimiter.h"
#include <algorithm>

namespace zl::dsp {

namespace {
    // バンド固有のリリース時定数設計
    //   fast は Auto Release の fast envelope（floor）として働く。
    //   slow は dual-envelope の遅い側。min(fast, slow) で抑制量が決まる。
    struct BandReleaseSpec { float fastMs; float slowMs; };
    constexpr BandReleaseSpec kBandRelease[MultibandLimiter::kNumBands] = {
        // Low  (fc ~50 Hz)
        { 20.0f, 250.0f },
        // Mid  (fc ~700 Hz)
        {  5.0f, 150.0f },
        // High (fc ~10 kHz)
        {  1.0f,  80.0f },
    };
}

void MultibandLimiter::prepare(double sampleRate, int numChannelsIn, int maxBlockSize)
{
    preparedChannels = std::max(1, numChannelsIn);
    preparedBlock    = std::max(1, maxBlockSize);

    crossover.prepare(sampleRate, preparedChannels);
    crossover.setCrossoverFrequencies(kDefaultCrossoverLowHz, kDefaultCrossoverHighHz);

    for (auto& limiter : bandLimiters)
        limiter.prepare(sampleRate, preparedChannels);

    // バンド固有のリリース設定と Auto Release 強制 ON
    configureBandReleases();

    // 作業バッファを前もって最大ブロックサイズで確保
    lowBuf .setSize(preparedChannels, preparedBlock, false, false, true);
    midBuf .setSize(preparedChannels, preparedBlock, false, false, true);
    highBuf.setSize(preparedChannels, preparedBlock, false, false, true);
}

void MultibandLimiter::reset()
{
    crossover.reset();
    for (auto& limiter : bandLimiters)
        limiter.reset();
}

void MultibandLimiter::setThresholdDb(float thresholdDb)
{
    currentThresholdDb = thresholdDb;
    for (auto& limiter : bandLimiters)
        limiter.setThresholdDb(thresholdDb);
}

void MultibandLimiter::setCrossoverFrequencies(float lowHz, float highHz)
{
    crossover.setCrossoverFrequencies(lowHz, highHz);
}

void MultibandLimiter::configureBandReleases()
{
    for (int b = 0; b < kNumBands; ++b)
    {
        bandLimiters[b].setReleaseMs    (kBandRelease[b].fastMs);
        bandLimiters[b].setSlowReleaseMs(kBandRelease[b].slowMs);
        bandLimiters[b].setAutoReleaseEnabled(true);
    }
}

float MultibandLimiter::processBlock(juce::AudioBuffer<float>& buffer) noexcept
{
    const int channels = buffer.getNumChannels();
    const int n        = buffer.getNumSamples();
    if (channels <= 0 || n <= 0)
        return 1.0f;

    // 3 バンド分解（入力は壊さない）
    crossover.processBlock(buffer, lowBuf, midBuf, highBuf);

    // バンドごとに独立リミット
    const float gLow  = bandLimiters[0].processBlock(lowBuf);
    const float gMid  = bandLimiters[1].processBlock(midBuf);
    const float gHigh = bandLimiters[2].processBlock(highBuf);

    // サム（in-place で buffer に書き戻す）
    for (int ch = 0; ch < channels; ++ch)
    {
        auto*       out = buffer.getWritePointer(ch);
        const auto* l   = lowBuf .getReadPointer(ch);
        const auto* m   = midBuf .getReadPointer(ch);
        const auto* h   = highBuf.getReadPointer(ch);
        for (int i = 0; i < n; ++i)
            out[i] = l[i] + m[i] + h[i];
    }

    return std::min({ gLow, gMid, gHigh });
}

} // namespace zl::dsp
