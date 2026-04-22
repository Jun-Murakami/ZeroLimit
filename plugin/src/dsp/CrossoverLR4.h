#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <array>

namespace zl::dsp {

// 可変バンド数 Linkwitz-Riley 4th-order IIR crossover（ゼロレイテンシー、3 / 4 / 5 バンド）
//
// トポロジ（tree cascade、バンド数 N のとき N-1 段）:
//   rest_{-1} = Input
//   stage i:  rest_{i-1} → {B_i = LP_LR4@c_i(rest_{i-1}),  rest_i = HP_LR4@c_i(rest_{i-1})}
//   最後のバンドは rest_{N-2} をそのまま HP 分岐した結果（B_{N-1}）。
//
// 位相アライメント:
//   バンド B_b が通過していないクロスオーバー c_k（k > b）について、allpass AP_LR4@c_k を適用。
//   これにより全バンド合計が
//     Σ B = AP_{N-2} · AP_{N-3} · ... · AP_0  (Input)
//   となり、振幅フラット（位相回転のみ）の allpass 応答として再構成される。
//
// バンド B_b に必要な AP 段数:
//   - B_0          : N-2 個（c_1, c_2, ..., c_{N-2}）
//   - B_1          : N-3 個（c_2, c_3, ..., c_{N-2}）
//   - ...
//   - B_{N-3}      : 1 個 （c_{N-2}）
//   - B_{N-2}, B_{N-1} : 0 個
class CrossoverLR4
{
public:
    static constexpr int kMaxBands      = 5;
    static constexpr int kMaxCrossovers = kMaxBands - 1; // 4
    static constexpr int kMaxChannels   = 2;

    void prepare(double sampleRate, int numChannels);
    void reset();

    // バンド数と crossover 周波数を設定。numBands は [3, kMaxBands]、
    // crossovers は numBands - 1 個、昇順。
    void configure(int numBands, const float* crossovers);
    int  getCurrentBandCount() const noexcept { return currentBandCount; }

    // 入力を numBands バンドへ分解。bandOuts[0..numBands-1] に書き出す。
    // 残りの bandOuts[k] (k >= numBands) は未定義（触らない）。
    // input 自体は書き換えない。
    void processBlock(const juce::AudioBuffer<float>& input,
                      std::array<juce::AudioBuffer<float>, kMaxBands>& bandOuts) noexcept;

private:
    using Filter = juce::dsp::IIR::Filter<float>;
    using Coefs  = juce::dsp::IIR::Coefficients<float>;

    // BW2 の Q（= 1/√2 ≈ 0.70711）
    static constexpr float kButterQ = 0.7071067811865476f;

    // LR4（BW2 二段カスケード）の LP + HP ペア
    struct LR4Pair
    {
        // [channel][stage 0..1]
        std::array<std::array<Filter, 2>, kMaxChannels> lp;
        std::array<std::array<Filter, 2>, kMaxChannels> hp;

        void prepare(const juce::dsp::ProcessSpec& spec) noexcept
        {
            for (int ch = 0; ch < kMaxChannels; ++ch)
            {
                for (auto& s : lp[ch]) s.prepare(spec);
                for (auto& s : hp[ch]) s.prepare(spec);
            }
        }
        void reset() noexcept
        {
            for (int ch = 0; ch < kMaxChannels; ++ch)
            {
                for (auto& s : lp[ch]) s.reset();
                for (auto& s : hp[ch]) s.reset();
            }
        }
        void setCoefficients(Coefs::Ptr lpCoefs, Coefs::Ptr hpCoefs) noexcept
        {
            for (int ch = 0; ch < kMaxChannels; ++ch)
            {
                for (auto& s : lp[ch]) s.coefficients = lpCoefs;
                for (auto& s : hp[ch]) s.coefficients = hpCoefs;
            }
        }

        // LP を in-place で適用（LR4 = 2 段カスケード）
        void applyLP(juce::AudioBuffer<float>& buf, int nCh, int nSamples) noexcept
        {
            for (int ch = 0; ch < nCh; ++ch)
            {
                auto* d  = buf.getWritePointer(ch);
                auto& s0 = lp[ch][0];
                auto& s1 = lp[ch][1];
                for (int i = 0; i < nSamples; ++i)
                    d[i] = s1.processSample(s0.processSample(d[i]));
            }
        }
        // HP を in-place で適用
        void applyHP(juce::AudioBuffer<float>& buf, int nCh, int nSamples) noexcept
        {
            for (int ch = 0; ch < nCh; ++ch)
            {
                auto* d  = buf.getWritePointer(ch);
                auto& s0 = hp[ch][0];
                auto& s1 = hp[ch][1];
                for (int i = 0; i < nSamples; ++i)
                    d[i] = s1.processSample(s0.processSample(d[i]));
            }
        }
    };

    // 共有係数（crossover ごと、[0..kMaxCrossovers-1]）
    std::array<Coefs::Ptr, kMaxCrossovers> lpCoefs;
    std::array<Coefs::Ptr, kMaxCrossovers> hpCoefs;

    // クロスオーバー本体：分割用の LR4 ペア。splitPairs[k] は c_k で分割する。
    std::array<LR4Pair, kMaxCrossovers> splitPairs;

    // AP 位相アライメント用。apPairs[band b][j] は B_b の j-th AP ステージ。
    //  使う係数は lpCoefs[b + 1 + j] / hpCoefs[b + 1 + j]。
    //  band ∈ [0, kMaxBands-2), j ∈ [0, kMaxCrossovers - 1)
    std::array<std::array<LR4Pair, kMaxCrossovers - 1>, kMaxBands - 2> apPairs;

    double sampleRate  = 44100.0;
    int    numChannels = 2;
    int    currentBandCount = 3;
    std::array<float, kMaxCrossovers> crossoverFreqs{ 120.0f, 5000.0f, 0.0f, 0.0f };

    // 作業用
    juce::AudioBuffer<float> restBuf;      // rest_i のキャリアとして使い回す
    juce::AudioBuffer<float> bandRawBuf;   // B_b の AP 適用前バッファ
    juce::AudioBuffer<float> tmpA, tmpB;   // AP (LP+HP) 加算のための中間

    void updateCoefficients();
    void assignCoefficientsToAllPairs();
};

} // namespace zl::dsp
