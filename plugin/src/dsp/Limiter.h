#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>

namespace zl::dsp {

// ゼロレイテンシー・ブリックウォール・リミッター
// - ルックアヘッドを用いず、瞬間ピークに対してその場でゲインを落とす（＝ 0 sample latency）
// - アタックは即時、リリースは時定数ベースで滑らかに回復
// - スレッショルド（リニア振幅）を超えない保証のため、target = threshold / |x| を必要最小分だけ適用
// - 出力段でリンク（L/R 共通のゲインリダクション）してステレオ像を崩さない
class ZeroLatencyLimiter
{
public:
    void prepare(double sampleRate, int numChannels);
    void reset();

    // スレッショルド（dBFS, -40..0）
    void setThresholdDb(float thresholdDb);
    // リリース時定数（ms）。デフォルト 50ms 程度がブロードキャスト向き
    void setReleaseMs(float releaseMs);

    // 1 サンプルステレオ処理のためのインライン API
    // - sampleL/R を in-place で更新する
    // - 戻り値は適用されたゲインリダクション（リニア, 0..1, 1 = リダクションなし）
    float processSample(float& sampleL, float& sampleR) noexcept;

    // ブロック処理版（N チャネル対応）
    // - 各サンプルで全チャネル連動のゲインリダクションを計算・適用
    // - 区間中の最大リダクション（リニア, 0..1）を返す
    float processBlock(juce::AudioBuffer<float>& buffer) noexcept;

private:
    float thresholdLin = 1.0f;  // リニア振幅（0..1）。0dBFS = 1.0
    float releaseCoeff = 0.9995f;
    float currentGain  = 1.0f;   // 現在適用中のゲイン（1.0 で無リダクション）
    double currentSampleRate = 44100.0;
    float releaseMs = 50.0f;

    void updateReleaseCoeff();
};

} // namespace zl::dsp
