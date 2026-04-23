#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <vector>

#include "ParameterIDs.h"
#include "dsp/Limiter.h"
#include "dsp/MomentaryProcessor.h"
#include "dsp/MultibandLimiter.h"

class ZeroLimitAudioProcessor : public juce::AudioProcessor
{
public:
    ZeroLimitAudioProcessor();
    ~ZeroLimitAudioProcessor() override;

    // AudioProcessor 基本
    const juce::String getName() const override;
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const juce::AudioProcessor::BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    // エディタ
    bool hasEditor() const override;
    juce::AudioProcessorEditor* createEditor() override;

    // プロパティ
    double getTailLengthSeconds() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;

    // プログラム
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int) override;
    const juce::String getProgramName(int) override;
    void changeProgramName(int, const juce::String&) override;
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // パラメータアクセス
    juce::AudioProcessorValueTreeState& getState() { return parameters; }

    // メーター値（dBFS スケールで保持。UI から読み取る）
    //  - inputLevelLR: 入力段のトゥルーピーク相当（区間最大）
    //  - outputLevelLR: 出力段のトゥルーピーク相当（区間最大）
    //  - gainReductionDb: 区間中の最大ゲインリダクション（正値 dB, 0 = リダクションなし）
    std::atomic<float> inputLevelLeft  { -60.0f };
    std::atomic<float> inputLevelRight { -60.0f };
    std::atomic<float> outputLevelLeft { -60.0f };
    std::atomic<float> outputLevelRight{ -60.0f };
    std::atomic<float> gainReductionDb { 0.0f };

    // 区間最大の蓄積用（オーディオスレッドで更新 → UI タイマーで取り出し）
    std::atomic<float> inPeakAccumL { 0.0f };
    std::atomic<float> inPeakAccumR { 0.0f };
    std::atomic<float> outPeakAccumL{ 0.0f };
    std::atomic<float> outPeakAccumR{ 0.0f };
    std::atomic<float> minGainAccum { 1.0f };  // 区間最小ゲイン（= 最大リダクション）

    // 区間 RMS の蓄積（ブロックごとの RMS を最大値蓄積）
    std::atomic<float> inRmsAccumL { 0.0f };
    std::atomic<float> inRmsAccumR { 0.0f };
    std::atomic<float> outRmsAccumL{ 0.0f };
    std::atomic<float> outRmsAccumR{ 0.0f };

    // Momentary LKFS（UI タイマーから getMomentaryLKFS() で読む）
    zl::dsp::MomentaryProcessor inputMomentary;
    zl::dsp::MomentaryProcessor outputMomentary;

    // ================= Waveform display (Pro-L 風のオシロ表示) =================
    //  - 入力側の |L|,|R| マージ済みサンプルを "slice" 単位（約 200 Hz）でダウンサンプル。
    //  - slice ごとの値:
    //      peak      : slice 内の max(|L|,|R|) のリニア振幅
    //      minGain   : slice をまたぐブロックで適用された min gain（= 最大リダクション）
    //  - AbstractFifo + 固定長リングで、audio → UI へ wait-free に受け渡し。
    //  - 5 秒表示でも余裕を持つため 2048 slot 用意（200Hz × ~10s）。
    static constexpr int kWaveformFifoSize   = 2048;
    static constexpr double kWaveformSliceHz = 200.0;
    juce::AbstractFifo waveformFifo{ kWaveformFifoSize };
    std::vector<float> waveformPeakBuffer;      // size = kWaveformFifoSize
    std::vector<float> waveformMinGainBuffer;   // size = kWaveformFifoSize
    int   waveformSliceSize         = 220;      // = sampleRate / 200 で prepare 時に設定
    int   waveformSliceSampleCount  = 0;        // audio thread のみが書く
    float waveformSlicePeakAccum    = 0.0f;     // audio thread のみ
    float waveformSliceMinGainAccum = 1.0f;     // audio thread のみ

    // UI から現在のサンプリングレートを取得する用（slice レートの再計算不要のため emit だけ）
    std::atomic<float> waveformSliceHz{ static_cast<float>(kWaveformSliceHz) };

    // audio thread から呼ばれる：1 サンプルぶんの入力ピーク/ゲインを slice に積む
    void pushWaveformSample(float absPeakSample, float blockMinGainLin) noexcept;

private:
    juce::AudioProcessorValueTreeState parameters;
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    zl::dsp::ZeroLatencyLimiter limiter;

    // マルチバンドモード専用。3 バンド LR4 分解 + 各バンド独立リミッタ。
    //  サム後には上の `limiter` を最終セーフティとしてもう一度適用する。
    zl::dsp::MultibandLimiter multibandLimiter;

    // processBlock で入力信号を保持するための作業用バッファ
    //  - 出力段リミッタが in-place で書き換えるため、入力側メータ/Momentary 用に複製を取る
    juce::AudioBuffer<float> inputCopyBuffer;

    // 波形表示用 per-sample gain スクラッチ:
    //  - waveformGainScratchA: limiter / multibandLimiter からの per-sample gain
    //  - waveformGainScratchB: safety limiter の per-sample gain（multi モード時のみ使用）
    //  - prepare 時に maxBlockSize で確保、processBlock では追加 alloc しない
    std::vector<float> waveformGainScratchA;
    std::vector<float> waveformGainScratchB;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ZeroLimitAudioProcessor)
};
