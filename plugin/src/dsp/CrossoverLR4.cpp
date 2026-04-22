#include "CrossoverLR4.h"
#include <algorithm>

namespace zl::dsp {

void CrossoverLR4::prepare(double sr, int channelsIn)
{
    sampleRate  = sr > 0.0 ? sr : 44100.0;
    numChannels = std::min(kMaxChannels, std::max(1, channelsIn));

    juce::dsp::ProcessSpec spec{};
    spec.sampleRate       = sampleRate;
    spec.maximumBlockSize = 1;       // IIR::Filter は reset のみ。サンプル処理で使う。
    spec.numChannels      = 1;

    auto prepAll = [&](auto& stagesArr)
    {
        for (int ch = 0; ch < kMaxChannels; ++ch)
            for (auto& stage : stagesArr[ch])
                stage.prepare(spec);
    };
    prepAll(lpLowStages);
    prepAll(hpLowStages);
    prepAll(lpHighStages);
    prepAll(hpHighStages);
    prepAll(lpHighOnLow);
    prepAll(hpHighOnLow);

    updateCoefficients();
    assignCoefficientsToStages();
    reset();
}

void CrossoverLR4::reset()
{
    auto resetAll = [](auto& stagesArr)
    {
        for (int ch = 0; ch < kMaxChannels; ++ch)
            for (auto& stage : stagesArr[ch])
                stage.reset();
    };
    resetAll(lpLowStages);
    resetAll(hpLowStages);
    resetAll(lpHighStages);
    resetAll(hpHighStages);
    resetAll(lpHighOnLow);
    resetAll(hpHighOnLow);
}

void CrossoverLR4::setCrossoverFrequencies(float lowHz, float highHz)
{
    // fLow < fHigh を保証し、サンプルレートのナイキストより十分下に収める。
    const float nyq   = static_cast<float>(sampleRate) * 0.5f;
    const float safeU = nyq * 0.9f;
    fLow  = std::clamp(lowHz,  20.0f, safeU);
    fHigh = std::clamp(highHz, fLow + 10.0f, safeU);
    updateCoefficients();
    assignCoefficientsToStages();
}

void CrossoverLR4::updateCoefficients()
{
    // LR4 の各段は BW2（Q = 1/√2）。これを 2 段カスケードすると 4 次の LR となる。
    lpLowCoefs  = Coefs::makeLowPass (sampleRate, fLow,  kButterQ);
    hpLowCoefs  = Coefs::makeHighPass(sampleRate, fLow,  kButterQ);
    lpHighCoefs = Coefs::makeLowPass (sampleRate, fHigh, kButterQ);
    hpHighCoefs = Coefs::makeHighPass(sampleRate, fHigh, kButterQ);
}

void CrossoverLR4::assignCoefficientsToStages()
{
    auto assignAll = [](auto& stagesArr, Coefs::Ptr coefs)
    {
        for (int ch = 0; ch < kMaxChannels; ++ch)
            for (auto& stage : stagesArr[ch])
                stage.coefficients = coefs;
    };
    assignAll(lpLowStages,  lpLowCoefs);
    assignAll(hpLowStages,  hpLowCoefs);
    assignAll(lpHighStages, lpHighCoefs);
    assignAll(hpHighStages, hpHighCoefs);
    assignAll(lpHighOnLow,  lpHighCoefs);
    assignAll(hpHighOnLow,  hpHighCoefs);
}

void CrossoverLR4::processStageInPlace(std::array<std::array<Filter, 2>, kMaxChannels>& stages,
                                       juce::AudioBuffer<float>& buf) noexcept
{
    const int channels = std::min(numChannels, buf.getNumChannels());
    const int n        = buf.getNumSamples();
    for (int ch = 0; ch < channels; ++ch)
    {
        auto* data = buf.getWritePointer(ch);
        auto& s0   = stages[ch][0];
        auto& s1   = stages[ch][1];
        for (int i = 0; i < n; ++i)
            data[i] = s1.processSample(s0.processSample(data[i]));
    }
}

void CrossoverLR4::processBlock(const juce::AudioBuffer<float>& input,
                                juce::AudioBuffer<float>& lowOut,
                                juce::AudioBuffer<float>& midOut,
                                juce::AudioBuffer<float>& highOut) noexcept
{
    const int channels = std::min(numChannels, input.getNumChannels());
    const int n        = input.getNumSamples();
    if (channels <= 0 || n <= 0)
        return;

    // 作業バッファを確保（既存割当を再利用）
    auto ensure = [&](juce::AudioBuffer<float>& b)
    {
        if (b.getNumChannels() != channels || b.getNumSamples() < n)
            b.setSize(channels, n, false, false, true);
    };
    ensure(midHighBuffer);
    ensure(lowRawBuffer);
    ensure(lowAPLowBuffer);
    ensure(lowAPHighBuffer);
    if (lowOut.getNumChannels()  != channels || lowOut.getNumSamples()  < n) lowOut.setSize (channels, n, false, false, true);
    if (midOut.getNumChannels()  != channels || midOut.getNumSamples()  < n) midOut.setSize (channels, n, false, false, true);
    if (highOut.getNumChannels() != channels || highOut.getNumSamples() < n) highOut.setSize(channels, n, false, false, true);

    // 1) Input を lowRaw / midHigh にコピーしてからそれぞれ LR4 を適用
    for (int ch = 0; ch < channels; ++ch)
    {
        lowRawBuffer .copyFrom(ch, 0, input, ch, 0, n);
        midHighBuffer.copyFrom(ch, 0, input, ch, 0, n);
    }
    processStageInPlace(lpLowStages, lowRawBuffer);   // Low_raw = LP_LR4@fLow(Input)
    processStageInPlace(hpLowStages, midHighBuffer);  // MidHigh = HP_LR4@fLow(Input)

    // 2) MidHigh を Mid / High に分ける
    for (int ch = 0; ch < channels; ++ch)
    {
        midOut .copyFrom(ch, 0, midHighBuffer, ch, 0, n);
        highOut.copyFrom(ch, 0, midHighBuffer, ch, 0, n);
    }
    processStageInPlace(lpHighStages, midOut);   // Mid  = LP_LR4@fHigh(MidHigh)
    processStageInPlace(hpHighStages, highOut);  // High = HP_LR4@fHigh(MidHigh)

    // 3) Low_raw に allpass@fHigh を適用して位相アライメント
    //    AP_LR4@fHigh(x) = LP_LR4@fHigh(x) + HP_LR4@fHigh(x)
    for (int ch = 0; ch < channels; ++ch)
    {
        lowAPLowBuffer .copyFrom(ch, 0, lowRawBuffer, ch, 0, n);
        lowAPHighBuffer.copyFrom(ch, 0, lowRawBuffer, ch, 0, n);
    }
    processStageInPlace(lpHighOnLow, lowAPLowBuffer);
    processStageInPlace(hpHighOnLow, lowAPHighBuffer);

    // Low = LP成分 + HP成分
    for (int ch = 0; ch < channels; ++ch)
    {
        auto*       out = lowOut.getWritePointer(ch);
        const auto* lp  = lowAPLowBuffer .getReadPointer(ch);
        const auto* hp  = lowAPHighBuffer.getReadPointer(ch);
        for (int i = 0; i < n; ++i)
            out[i] = lp[i] + hp[i];
    }
}

} // namespace zl::dsp
