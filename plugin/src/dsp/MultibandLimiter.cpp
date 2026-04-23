#include "MultibandLimiter.h"
#include <algorithm>

namespace zl::dsp {

namespace {
    struct BandSpec { float fastMs; float slowMs; };

    // --- 3-band (120 Hz / 5 kHz) ---
    constexpr float kCrossovers3[2] = { 120.0f, 5000.0f };
    constexpr BandSpec kBands3[3] = {
        { 20.0f, 250.0f },  // Low   (<120)
        {  5.0f, 150.0f },  // Mid   (120-5k)   ← 声帯域
        {  1.0f,  80.0f },  // High  (>5k)
    };

    // --- 4-band (Steinberg: 150 / 5k / 15k) ---
    constexpr float kCrossovers4[3] = { 150.0f, 5000.0f, 15000.0f };
    constexpr BandSpec kBands4[4] = {
        { 20.0f, 250.0f },  // Low      (<150)
        {  5.0f, 150.0f },  // LowMid   (150-5k)   ← 声帯域
        {  1.0f,  80.0f },  // HighMid  (5-15k)
        {  0.5f,  50.0f },  // Air      (>15k)
    };

    // --- 5-band (UA all-round: 80 / 250 / 1k / 5k) ---
    constexpr float kCrossovers5[4] = { 80.0f, 250.0f, 1000.0f, 5000.0f };
    constexpr BandSpec kBands5[5] = {
        { 30.0f, 300.0f },  // Sub      (<80)
        { 15.0f, 200.0f },  // Bass     (80-250)
        {  5.0f, 120.0f },  // LowMid   (250-1k)
        {  2.0f, 100.0f },  // MidHigh  (1k-5k)
        {  1.0f,  80.0f },  // High     (>5k)
    };

    void applyBandSpec(ZeroLatencyLimiter& lim, const BandSpec& spec)
    {
        lim.setReleaseMs    (spec.fastMs);
        lim.setSlowReleaseMs(spec.slowMs);
        lim.setAutoReleaseEnabled(true);
    }
}

int MultibandLimiter::getNumBands() const noexcept
{
    switch (currentMode)
    {
        case Mode::Band3: return 3;
        case Mode::Band4: return 4;
        case Mode::Band5: return 5;
    }
    return 3;
}

void MultibandLimiter::prepare(double sampleRate, int numChannelsIn, int maxBlockSize)
{
    preparedChannels = std::max(1, numChannelsIn);
    preparedBlock    = std::max(1, maxBlockSize);

    for (auto& crossover : crossovers)
        crossover.prepare(sampleRate, preparedChannels, preparedBlock);

    crossovers[static_cast<int>(Mode::Band3)].configure(3, kCrossovers3);
    crossovers[static_cast<int>(Mode::Band4)].configure(4, kCrossovers4);
    crossovers[static_cast<int>(Mode::Band5)].configure(5, kCrossovers5);

    for (auto& limiter : bandLimiters)
        limiter.prepare(sampleRate, preparedChannels);

    for (auto& b : bandBufs)
        b.setSize(preparedChannels, preparedBlock, false, false, true);

    // per-sample gain 集計用スクラッチ（バンド限定器から受け取るサイズ）
    bandGainScratch.assign(static_cast<size_t>(preparedBlock), 1.0f);

    // 初期モード反映（既定は Band3）
    configureForMode(currentMode);
}

void MultibandLimiter::reset()
{
    for (auto& crossover : crossovers)
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

void MultibandLimiter::setMode(Mode mode)
{
    if (mode == currentMode) return;
    currentMode = mode;
    configureForMode(mode);
    // バンド再構成で切替前の内部状態はリセットしておく（ちらつき回避）
    getActiveCrossover().reset();
    for (auto& limiter : bandLimiters)
        limiter.reset();
}

void MultibandLimiter::configureForMode(Mode mode)
{
    switch (mode)
    {
        case Mode::Band3:
            for (int i = 0; i < 3; ++i) applyBandSpec(bandLimiters[i], kBands3[i]);
            break;
        case Mode::Band4:
            for (int i = 0; i < 4; ++i) applyBandSpec(bandLimiters[i], kBands4[i]);
            break;
        case Mode::Band5:
            for (int i = 0; i < 5; ++i) applyBandSpec(bandLimiters[i], kBands5[i]);
            break;
    }
}

CrossoverLR4& MultibandLimiter::getActiveCrossover() noexcept
{
    return crossovers[static_cast<int>(currentMode)];
}

float MultibandLimiter::processBlock(juce::AudioBuffer<float>& buffer, float* gainOut) noexcept
{
    const int channels = buffer.getNumChannels();
    const int n        = buffer.getNumSamples();
    if (channels <= 0 || n <= 0)
        return 1.0f;

    const int N = getNumBands();

    for (int i = 0; i < N; ++i)
    {
        if (bandBufs[i].getNumChannels() != channels || bandBufs[i].getNumSamples() != n)
            bandBufs[i].setSize(channels, n, false, false, true);
    }

    // スクラッチバッファサイズ確認（prepare 時の maxBlockSize を上回るケースへの保険）
    if (gainOut && static_cast<int>(bandGainScratch.size()) < n)
        bandGainScratch.resize(static_cast<size_t>(n), 1.0f);

    // gainOut を 1.0 で初期化
    if (gainOut)
    {
        for (int i = 0; i < n; ++i) gainOut[i] = 1.0f;
    }

    // バンド分解
    getActiveCrossover().processBlock(buffer, bandBufs);

    // バンドごとに独立リミット、最小ゲインを集計
    float minGain = 1.0f;
    for (int i = 0; i < N; ++i)
    {
        float* perSampleOut = gainOut ? bandGainScratch.data() : nullptr;
        const float g = bandLimiters[i].processBlock(bandBufs[i], perSampleOut);
        if (g < minGain) minGain = g;
        // gainOut にバンド間の最小 gain を合成
        if (gainOut)
        {
            for (int s = 0; s < n; ++s)
                if (perSampleOut[s] < gainOut[s]) gainOut[s] = perSampleOut[s];
        }
    }

    // サム（in-place で buffer に書き戻す）
    for (int ch = 0; ch < channels; ++ch)
    {
        auto* out = buffer.getWritePointer(ch);
        // 最初のバンドで初期化
        const auto* src0 = bandBufs[0].getReadPointer(ch);
        for (int i = 0; i < n; ++i)
            out[i] = src0[i];
        // 残りを加算
        for (int b = 1; b < N; ++b)
        {
            const auto* src = bandBufs[b].getReadPointer(ch);
            for (int i = 0; i < n; ++i)
                out[i] += src[i];
        }
    }

    return minGain;
}

} // namespace zl::dsp
