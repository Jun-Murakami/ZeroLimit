#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>

#include "CrossoverLR4.h"
#include "Limiter.h"

namespace zl::dsp {

// 3 バンド・マルチバンド・ブリックウォール・リミッター（ゼロレイテンシー）
//
//   Input → CrossoverLR4 → Low / Mid / High
//         → 各バンドに独立リミッタ（Auto Release 強制、バンド固有の時定数）
//         → サム
//
// 最終セーフティリミッタはここには含めない（呼び出し側で既存 ZeroLatencyLimiter を
//  サム後に適用する）。これによりバンド合成後の位相合成オーバーシュートもクランプされる。
//
// バンド固有の時定数設計（経験則：最小リリース ≥ 2-3 × バンド中心周波数の周期）:
//   - Low    (fc ≈ 50 Hz ): slow 250 ms,  fast floor 20 ms
//   - Mid    (fc ≈ 700 Hz): slow 150 ms,  fast floor 5  ms
//   - High   (fc ≈ 10 kHz): slow 80  ms,  fast floor 1  ms
class MultibandLimiter
{
public:
    static constexpr int kNumBands = 3;
    static constexpr float kDefaultCrossoverLowHz  = 120.0f;
    static constexpr float kDefaultCrossoverHighHz = 5000.0f;

    void prepare(double sampleRate, int numChannels, int maxBlockSize);
    void reset();

    void setThresholdDb(float thresholdDb);
    void setCrossoverFrequencies(float lowHz, float highHz);

    // in-place 処理。戻り値は区間中に観測された最小ゲイン（= 最大リダクション, 0..1）。
    // バンド間の最小値（= 最もリダクションが深かったバンドの値）を返す。
    float processBlock(juce::AudioBuffer<float>& buffer) noexcept;

private:
    CrossoverLR4 crossover;
    std::array<ZeroLatencyLimiter, kNumBands> bandLimiters;

    juce::AudioBuffer<float> lowBuf;
    juce::AudioBuffer<float> midBuf;
    juce::AudioBuffer<float> highBuf;

    int   preparedChannels = 2;
    int   preparedBlock    = 0;
    float currentThresholdDb = 0.0f;

    void configureBandReleases();
};

} // namespace zl::dsp
