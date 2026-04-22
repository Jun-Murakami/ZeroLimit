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

    // THRESHOLD: -40..0 dBFS（既定 -1dB ＝ ブロードキャスト向け）
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        zl::id::THRESHOLD,
        "Threshold",
        juce::NormalisableRange<float>(-40.0f, 0.0f, 0.1f),
        -1.0f,
        juce::AudioParameterFloatAttributes().withLabel("dB")));

    // OUTPUT_GAIN: -24..+24 dB（既定 0）
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        zl::id::OUTPUT_GAIN,
        "Output Gain",
        juce::NormalisableRange<float>(-24.0f, 24.0f, 0.1f),
        0.0f,
        juce::AudioParameterFloatAttributes().withLabel("dB")));

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

void ZeroLimitAudioProcessor::prepareToPlay(double sampleRate, int /*samplesPerBlock*/)
{
    limiter.prepare(sampleRate, getTotalNumOutputChannels());
    limiter.setReleaseMs(50.0f); // ブロードキャスト向けデフォルト

    if (auto* p = parameters.getRawParameterValue(zl::id::THRESHOLD.getParamID()))
        limiter.setThresholdDb(p->load());
}

void ZeroLimitAudioProcessor::releaseResources()
{
    limiter.reset();
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
    limiter.setThresholdDb(thresholdDb);

    // --- 入力段メータ ---
    float inPeakL = 0.0f, inPeakR = 0.0f;
    {
        auto* l = buffer.getReadPointer(0);
        auto* r = buffer.getReadPointer(std::min(1, numChannels - 1));
        for (int i = 0; i < numSamples; ++i)
        {
            inPeakL = std::max(inPeakL, std::abs(l[i]));
            inPeakR = std::max(inPeakR, std::abs(r[i]));
        }
    }
    atomicMaxFloat(inPeakAccumL, inPeakL);
    atomicMaxFloat(inPeakAccumR, inPeakR);

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

    // --- 出力段メータ ---
    float outPeakL = 0.0f, outPeakR = 0.0f;
    {
        auto* l = buffer.getReadPointer(0);
        auto* r = buffer.getReadPointer(std::min(1, numChannels - 1));
        for (int i = 0; i < numSamples; ++i)
        {
            outPeakL = std::max(outPeakL, std::abs(l[i]));
            outPeakR = std::max(outPeakR, std::abs(r[i]));
        }
    }
    atomicMaxFloat(outPeakAccumL, outPeakL);
    atomicMaxFloat(outPeakAccumR, outPeakR);
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
