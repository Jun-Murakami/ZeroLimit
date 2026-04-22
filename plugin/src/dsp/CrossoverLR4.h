#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <array>

namespace zl::dsp {

// 3-band Linkwitz-Riley 4th-order IIR crossover（ゼロレイテンシー）
//
// トポロジ（tree cascade）:
//   Input → LR4@fLow → {Low_raw, MidHigh}
//           MidHigh  → LR4@fHigh → {Mid, High}
//
// LR4 は BW2 を 2 段カスケードして構成する（4 次）。
// LR4 LP + LR4 HP = 全帯域で振幅 1 の allpass 応答になる（位相のみ回転）。
//
// 位相アライメント:
//   素朴な tree 分割だと Low には LR4@fHigh を通過していない分、他の 2 バンドと位相が揃わない。
//   これを補償するため Low には LR4 allpass@fHigh を適用する
//   （= LP_LR4@fHigh(Low_raw) + HP_LR4@fHigh(Low_raw)）。
//   これで 3 バンドの和は
//     AP_LR4@fHigh · (LP_LR4@fLow + HP_LR4@fLow) = AP_LR4@fHigh · AP_LR4@fLow
//   すなわち振幅フラットな allpass 応答となる（バンドゲインが等しいとき入力を再構成）。
//
// 典型運用:
//   fLow  = 120 Hz  （声の F0 帯を Mid に残す）
//   fHigh = 5000 Hz （声のプレゼンス帯を Mid に残す）
class CrossoverLR4
{
public:
    static constexpr int kMaxChannels = 2;

    void prepare(double sampleRate, int numChannels);
    void reset();

    // クロスオーバー周波数の設定（Hz）。prepare 後に呼ぶ。
    void setCrossoverFrequencies(float lowHz, float highHz);

    // 3 バンドへ分解する。出力バッファは事前に input と同じサイズに確保されていること。
    // input 自体は書き換えない。
    void processBlock(const juce::AudioBuffer<float>& input,
                      juce::AudioBuffer<float>& lowOut,
                      juce::AudioBuffer<float>& midOut,
                      juce::AudioBuffer<float>& highOut) noexcept;

private:
    using Filter = juce::dsp::IIR::Filter<float>;
    using Coefs  = juce::dsp::IIR::Coefficients<float>;

    double sampleRate  = 44100.0;
    int    numChannels = 2;
    float  fLow        = 120.0f;
    float  fHigh       = 5000.0f;

    // BW2 の Q（= 1/√2 ≈ 0.70711）
    static constexpr float kButterQ = 0.7071067811865476f;

    // 共有係数（パラメータ変更時に作り直す）
    Coefs::Ptr lpLowCoefs;
    Coefs::Ptr hpLowCoefs;
    Coefs::Ptr lpHighCoefs;
    Coefs::Ptr hpHighCoefs;

    // フィルタ状態は [channel][stage 0..1]。LR4 = BW2 を 2 段カスケード。
    // Input → Low_raw / MidHigh の分割
    std::array<std::array<Filter, 2>, kMaxChannels> lpLowStages;   // Input → Low_raw
    std::array<std::array<Filter, 2>, kMaxChannels> hpLowStages;   // Input → MidHigh

    // MidHigh → Mid / High の分割
    std::array<std::array<Filter, 2>, kMaxChannels> lpHighStages;  // MidHigh → Mid
    std::array<std::array<Filter, 2>, kMaxChannels> hpHighStages;  // MidHigh → High

    // Low_raw の位相アライメント用（allpass@fHigh = LP_LR4@fHigh + HP_LR4@fHigh）
    std::array<std::array<Filter, 2>, kMaxChannels> lpHighOnLow;   // Low_raw → AP の LP 成分
    std::array<std::array<Filter, 2>, kMaxChannels> hpHighOnLow;   // Low_raw → AP の HP 成分

    // 作業用
    juce::AudioBuffer<float> midHighBuffer;
    juce::AudioBuffer<float> lowRawBuffer;
    juce::AudioBuffer<float> lowAPLowBuffer;
    juce::AudioBuffer<float> lowAPHighBuffer;

    void updateCoefficients();
    void assignCoefficientsToStages();
    void processStageInPlace(std::array<std::array<Filter, 2>, kMaxChannels>& stages,
                             juce::AudioBuffer<float>& buf) noexcept;
};

} // namespace zl::dsp
