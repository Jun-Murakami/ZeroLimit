#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <vector>

#include "CrossoverLR4.h"
#include "Limiter.h"

namespace zl::dsp {

// 可変バンド（3 / 4 / 5）マルチバンド・ブリックウォール・リミッター（ゼロレイテンシー）
//
//   Input → CrossoverLR4（動的 3/4/5 バンド） → 各バンドに独立リミッタ → サム
//
// 最終セーフティリミッタはここに含めない（呼び出し側で既存 ZeroLatencyLimiter を
// サム後に適用する）。位相合成オーバーシュートはそこで吸収される。
//
// バンド数ごとの設計（固定値・ゼロコンフィグ）:
//
// [3-band] 放送寄り。声を Mid バンドに閉じ込める。
//   crossovers: 120 Hz / 5 kHz
//   Low  (<120 Hz)     : fast 20 ms,  slow 250 ms
//   Mid  (120 Hz-5 kHz): fast 5  ms,  slow 150 ms   ← 声帯域
//   High (>5 kHz)      : fast 1  ms,  slow 80  ms
//
// [4-band] Steinberg 準拠。声を Low-Mid に閉じ込め + Air 分離。
//   crossovers: 150 Hz / 5 kHz / 15 kHz
//   Low      (<150 Hz)     : fast 20  ms, slow 250 ms
//   LowMid   (150 Hz-5 kHz): fast 5   ms, slow 150 ms   ← 声帯域
//   HighMid  (5-15 kHz)    : fast 1   ms, slow 80  ms
//   Air      (>15 kHz)     : fast 0.5 ms, slow 50  ms
//
// [5-band] UA 準拠。音楽マスタリング志向。声は Mid バンドを中心に扱われるが F1/F2 が分割される。
//   crossovers: 80 / 250 / 1000 / 5000 Hz
//   Sub      (<80)         : fast 30 ms,  slow 300 ms
//   Bass     (80-250)      : fast 15 ms,  slow 200 ms
//   LowMid   (250-1k)      : fast 5  ms,  slow 120 ms
//   MidHigh  (1k-5k)       : fast 2  ms,  slow 100 ms
//   High     (>5k)         : fast 1  ms,  slow 80  ms
class MultibandLimiter
{
public:
    static constexpr int kMaxBands = CrossoverLR4::kMaxBands; // 5

    enum class Mode { Band3 = 0, Band4 = 1, Band5 = 2 };

    void prepare(double sampleRate, int numChannels, int maxBlockSize);
    void reset();

    void setThresholdDb(float thresholdDb);
    // 3 / 4 / 5 バンドの切替。band count とそれぞれの既定 crossover、時定数がセットで適用される。
    void setMode(Mode mode);
    Mode getMode() const noexcept { return currentMode; }
    int  getNumBands() const noexcept;

    // in-place 処理。戻り値は区間中に観測された最小ゲイン（= 最大リダクション, 0..1）。
    // バンド間の最小値（= 最もリダクションが深かったバンドの値）を返す。
    //
    // gainOut != nullptr なら、各サンプルで観測された「バンド間最小 gain」（リニア 0..1）を書き出す。
    //  バンドはサムされるので output/input 比は位相シフトとバンド和で意味を成さないが、
    //  min-across-bands は視覚的に「最も深く削られたバンドがどれだけ削ったか」を示す妥当な指標。
    //  配列長は最低でも `buffer.getNumSamples()` 必要。
    float processBlock(juce::AudioBuffer<float>& buffer, float* gainOut = nullptr) noexcept;

private:
    std::array<CrossoverLR4, 3> crossovers;
    std::array<ZeroLatencyLimiter, kMaxBands> bandLimiters;
    std::array<juce::AudioBuffer<float>, kMaxBands> bandBufs;
    // per-sample gain 集計用スクラッチ（bandLimiter から受け取って min 合成）
    std::vector<float> bandGainScratch;

    int   preparedChannels  = 2;
    int   preparedBlock     = 0;
    float currentThresholdDb = 0.0f;
    Mode  currentMode        = Mode::Band3;

    void configureForMode(Mode mode);
    CrossoverLR4& getActiveCrossover() noexcept;
};

} // namespace zl::dsp
