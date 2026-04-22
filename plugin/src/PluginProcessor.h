#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>
#include <atomic>

#include "ParameterIDs.h"
#include "dsp/Limiter.h"

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

private:
    juce::AudioProcessorValueTreeState parameters;
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    zl::dsp::ZeroLatencyLimiter limiter;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ZeroLimitAudioProcessor)
};
