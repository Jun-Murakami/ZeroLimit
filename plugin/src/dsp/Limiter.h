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
    // 手動リリース時定数（ms）。Auto Release OFF 時に使用。
    //  Auto Release ON 時は fast envelope の時定数として流用される（= fast floor）。
    void setReleaseMs(float releaseMs);
    // Slow envelope の時定数（ms）。Auto Release 計算に使われる。
    //  Single-band の既定値は 150 ms だが、マルチバンドではバンドごとに上書きする。
    void setSlowReleaseMs(float ms);
    // Auto Release モード。ON 時は fast/slow envelope の min を適用し、
    // 手動リリース時定数は無視される（内部は fast envelope として流用）。
    void setAutoReleaseEnabled(bool enabled) noexcept { autoReleaseEnabled = enabled; }

    // 1 サンプルステレオ処理のためのインライン API
    // - sampleL/R を in-place で更新する
    // - 戻り値は適用されたゲインリダクション（リニア, 0..1, 1 = リダクションなし）
    float processSample(float& sampleL, float& sampleR) noexcept;

    // ブロック処理版（N チャネル対応）
    // - 各サンプルで全チャネル連動のゲインリダクションを計算・適用
    // - 区間中の最大リダクション（リニア, 0..1）を返す
    // - gainOut != nullptr なら各サンプルで適用された gain（リニア, 0..1）を書き出す。
    //   配列長は最低でも `buffer.getNumSamples()` 必要。
    float processBlock(juce::AudioBuffer<float>& buffer, float* gainOut = nullptr) noexcept;

private:
    float thresholdLin = 1.0f;  // リニア振幅（0..1）。0dBFS = 1.0

    // Fast envelope（手動リリース設定に追従）
    float releaseCoeff = 0.9995f;
    float currentGain  = 1.0f;
    float releaseMs = 1.0f;

    // Slow envelope（Auto Release 用。既定 150 ms、setSlowReleaseMs で差し替え可）
    float slowReleaseCoeff = 0.9999f;
    float currentGainSlow  = 1.0f;
    float autoSlowReleaseMs = 150.0f;

    bool autoReleaseEnabled = true;

    double currentSampleRate = 44100.0;

    void updateReleaseCoeffs();
};

} // namespace zl::dsp
