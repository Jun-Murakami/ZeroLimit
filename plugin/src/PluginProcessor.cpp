#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cmath>
#include <algorithm>
#include <memory>
#include <vector>

namespace {
    // オーディオスレッド安全に "atomic<float> のこれまでの最大値" を更新する
    inline void atomicMaxFloat(std::atomic<float>& slot, float candidate) noexcept
    {
        float prev = slot.load(std::memory_order_relaxed);
        while (candidate > prev &&
               !slot.compare_exchange_weak(prev, candidate,
                                           std::memory_order_acq_rel,
                                           std::memory_order_relaxed))
        { /* retry */ }
    }

    inline void atomicMinFloat(std::atomic<float>& slot, float candidate) noexcept
    {
        float prev = slot.load(std::memory_order_relaxed);
        while (candidate < prev &&
               !slot.compare_exchange_weak(prev, candidate,
                                           std::memory_order_acq_rel,
                                           std::memory_order_relaxed))
        { /* retry */ }
    }
}

ZeroLimitAudioProcessor::ZeroLimitAudioProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput ("Input",  juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, juce::Identifier("ZeroLimit"), createParameterLayout())
{
}

ZeroLimitAudioProcessor::~ZeroLimitAudioProcessor() = default;

juce::AudioProcessorValueTreeState::ParameterLayout ZeroLimitAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // THRESHOLD: -30..0 dBFS（既定 0 dB ＝ バイパス相当）
    //  業界標準の Waves L2 に合わせ、下限は -30 dB とする。
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        zl::id::THRESHOLD,
        "Threshold",
        juce::NormalisableRange<float>(-30.0f, 0.0f, 0.1f),
        0.0f,
        juce::AudioParameterFloatAttributes().withLabel("dB")));

    // OUTPUT_GAIN: -30..0 dB（既定 0）
    //  リミッター後段のトリム。増幅方向はリミッタが許さないので下方向のみ。
    //  Threshold とレンジを統一（L2 準拠の -30 dB まで）。
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        zl::id::OUTPUT_GAIN,
        "Output Gain",
        juce::NormalisableRange<float>(-30.0f, 0.0f, 0.1f),
        0.0f,
        juce::AudioParameterFloatAttributes().withLabel("dB")));

    // METERING_MODE: Peak / RMS / Momentary（UI 表示の切替、非オートメーション想定）
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        zl::id::METERING_MODE,
        "Metering Mode",
        juce::StringArray{ "Peak", "RMS", "Momentary" },
        0));

    // RELEASE_MS: 0.01 .. 1000 ms、対数マッピング（各デケードが等しく分布）
    //  convert: t=0 -> 0.01, t=0.2 -> 0.1, t=0.4 -> 1.0, ..., t=1 -> 1000
    juce::NormalisableRange<float> releaseRange(
        0.01f, 1000.0f,
        [](float start, float end, float t)  { return start * std::pow(end / start, t); },
        [](float start, float end, float v)  { return std::log(v / start) / std::log(end / start); },
        [](float start, float end, float v)  { return juce::jlimit(start, end, v); });

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        zl::id::RELEASE_MS,
        "Release",
        releaseRange,
        1.0f,
        juce::AudioParameterFloatAttributes().withLabel("ms")));

    // AUTO_RELEASE: プログラム依存リリース。既定は ON（L1/L2 的な破綻しない動作）。
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        zl::id::AUTO_RELEASE,
        "Auto Release",
        true));

    return { params.begin(), params.end() };
}

const juce::String ZeroLimitAudioProcessor::getName() const { return JucePlugin_Name; }
bool ZeroLimitAudioProcessor::acceptsMidi() const           { return false; }
bool ZeroLimitAudioProcessor::producesMidi() const          { return false; }
bool ZeroLimitAudioProcessor::isMidiEffect() const          { return false; }
double ZeroLimitAudioProcessor::getTailLengthSeconds() const{ return 0.0; }

int ZeroLimitAudioProcessor::getNumPrograms() { return 1; }
int ZeroLimitAudioProcessor::getCurrentProgram() { return 0; }
void ZeroLimitAudioProcessor::setCurrentProgram(int) {}
const juce::String ZeroLimitAudioProcessor::getProgramName(int) { return {}; }
void ZeroLimitAudioProcessor::changeProgramName(int, const juce::String&) {}

void ZeroLimitAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    limiter.prepare(sampleRate, getTotalNumOutputChannels());

    if (auto* p = parameters.getRawParameterValue(zl::id::THRESHOLD.getParamID()))
        limiter.setThresholdDb(p->load());
    if (auto* p = parameters.getRawParameterValue(zl::id::RELEASE_MS.getParamID()))
        limiter.setReleaseMs(p->load());
    if (auto* p = parameters.getRawParameterValue(zl::id::AUTO_RELEASE.getParamID()))
        limiter.setAutoReleaseEnabled(p->load() > 0.5f);

    inputMomentary.prepareToPlay(sampleRate, samplesPerBlock);
    outputMomentary.prepareToPlay(sampleRate, samplesPerBlock);

    inputCopyBuffer.setSize(getTotalNumInputChannels(),
                            samplesPerBlock,
                            /*keepExistingContent*/ false,
                            /*clearExtraSpace*/     true,
                            /*avoidReallocating*/   false);
}

void ZeroLimitAudioProcessor::releaseResources()
{
    limiter.reset();
    inputMomentary.reset();
    outputMomentary.reset();
    inputCopyBuffer.setSize(0, 0);
}

bool ZeroLimitAudioProcessor::isBusesLayoutSupported(const juce::AudioProcessor::BusesLayout& layouts) const
{
    const auto& mainIn  = layouts.getMainInputChannelSet();
    const auto& mainOut = layouts.getMainOutputChannelSet();
    if (mainIn.isDisabled() || mainOut.isDisabled()) return false;
    if (mainIn != mainOut) return false;
    return mainOut == juce::AudioChannelSet::mono()
        || mainOut == juce::AudioChannelSet::stereo();
}

void ZeroLimitAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midi*/)
{
    juce::ScopedNoDenormals noDenormals;

    const int numChannels = buffer.getNumChannels();
    const int numSamples  = buffer.getNumSamples();

    if (numSamples <= 0 || numChannels <= 0)
        return;

    // パラメータ取得
    const float thresholdDb = parameters.getRawParameterValue(zl::id::THRESHOLD.getParamID())->load();
    const float outGainDb   = parameters.getRawParameterValue(zl::id::OUTPUT_GAIN.getParamID())->load();
    const float releaseMs   = parameters.getRawParameterValue(zl::id::RELEASE_MS.getParamID())->load();
    const bool  autoRel     = parameters.getRawParameterValue(zl::id::AUTO_RELEASE.getParamID())->load() > 0.5f;
    limiter.setThresholdDb(thresholdDb);
    limiter.setReleaseMs(releaseMs);
    limiter.setAutoReleaseEnabled(autoRel);

    // --- 入力信号のコピー（破壊前に取る）---
    if (inputCopyBuffer.getNumChannels() != numChannels
        || inputCopyBuffer.getNumSamples() < numSamples)
    {
        inputCopyBuffer.setSize(numChannels, numSamples, false, false, /*avoidReallocating=*/true);
    }
    for (int ch = 0; ch < numChannels; ++ch)
        inputCopyBuffer.copyFrom(ch, 0, buffer, ch, 0, numSamples);

    // --- 入力段メータ（Peak + RMS）---
    {
        auto* l = inputCopyBuffer.getReadPointer(0);
        auto* r = inputCopyBuffer.getReadPointer(std::min(1, numChannels - 1));
        float inPeakL = 0.0f, inPeakR = 0.0f;
        float sumSqL = 0.0f, sumSqR = 0.0f;
        for (int i = 0; i < numSamples; ++i)
        {
            const float al = std::abs(l[i]);
            const float ar = std::abs(r[i]);
            inPeakL = std::max(inPeakL, al);
            inPeakR = std::max(inPeakR, ar);
            sumSqL += l[i] * l[i];
            sumSqR += r[i] * r[i];
        }
        atomicMaxFloat(inPeakAccumL, inPeakL);
        atomicMaxFloat(inPeakAccumR, inPeakR);

        const float invN = 1.0f / static_cast<float>(numSamples);
        atomicMaxFloat(inRmsAccumL, std::sqrt(sumSqL * invN));
        atomicMaxFloat(inRmsAccumR, std::sqrt(sumSqR * invN));
    }

    // --- 入力 Momentary ---
    inputMomentary.processBlock(inputCopyBuffer);

    // --- リミッター ---
    const float minGain = limiter.processBlock(buffer);
    atomicMinFloat(minGainAccum, minGain);

    // --- 出力ゲイン ---
    const float outLin = std::pow(10.0f, outGainDb / 20.0f);
    if (std::abs(outLin - 1.0f) > 1.0e-6f)
    {
        for (int ch = 0; ch < numChannels; ++ch)
            buffer.applyGain(ch, 0, numSamples, outLin);
    }

    // --- 出力段メータ（Peak + RMS）---
    {
        auto* l = buffer.getReadPointer(0);
        auto* r = buffer.getReadPointer(std::min(1, numChannels - 1));
        float outPeakL = 0.0f, outPeakR = 0.0f;
        float sumSqL = 0.0f, sumSqR = 0.0f;
        for (int i = 0; i < numSamples; ++i)
        {
            const float al = std::abs(l[i]);
            const float ar = std::abs(r[i]);
            outPeakL = std::max(outPeakL, al);
            outPeakR = std::max(outPeakR, ar);
            sumSqL += l[i] * l[i];
            sumSqR += r[i] * r[i];
        }
        atomicMaxFloat(outPeakAccumL, outPeakL);
        atomicMaxFloat(outPeakAccumR, outPeakR);

        const float invN = 1.0f / static_cast<float>(numSamples);
        atomicMaxFloat(outRmsAccumL, std::sqrt(sumSqL * invN));
        atomicMaxFloat(outRmsAccumR, std::sqrt(sumSqR * invN));
    }

    // --- 出力 Momentary ---
    outputMomentary.processBlock(buffer);
}

bool ZeroLimitAudioProcessor::hasEditor() const { return true; }

juce::AudioProcessorEditor* ZeroLimitAudioProcessor::createEditor()
{
    return new ZeroLimitAudioProcessorEditor(*this);
}

void ZeroLimitAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    if (auto xml = parameters.copyState().createXml())
        copyXmlToBinary(*xml, destData);
}

void ZeroLimitAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
    {
        if (xml->hasTagName(parameters.state.getType()))
            parameters.replaceState(juce::ValueTree::fromXml(*xml));
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ZeroLimitAudioProcessor();
}
