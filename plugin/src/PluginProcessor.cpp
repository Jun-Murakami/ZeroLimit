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

    inline void sanitizeBufferFinite(juce::AudioBuffer<float>& buffer, int numChannels, int numSamples) noexcept
    {
        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* data = buffer.getWritePointer(ch);
            for (int i = 0; i < numSamples; ++i)
            {
                if (! std::isfinite(data[i]))
                    data[i] = 0.0f;
            }
        }
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

void ZeroLimitAudioProcessor::pushWaveformSample(float absPeakSample, float blockMinGainLin) noexcept
{
    // 1 slice 内の最大入力ピークと最小ゲインを累積。
    //  slice が完了したら FIFO に 1 スロット書き込む（リングが満杯でも 1 古い値を上書きする形）。
    if (absPeakSample > waveformSlicePeakAccum)
        waveformSlicePeakAccum = absPeakSample;
    if (blockMinGainLin < waveformSliceMinGainAccum)
        waveformSliceMinGainAccum = blockMinGainLin;

    if (++waveformSliceSampleCount >= waveformSliceSize)
    {
        // SPSC: 満杯のときは "最新の 1 slice を黙って捨てる"（消費側の read index を
        //  触らないことで read/write の race を避ける）。UI タイマーは 30Hz で最大数十 slice
        //  しか消費しないので、2048 スロットのリングなら現実には満杯にはならない。
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        waveformFifo.prepareToWrite(1, start1, size1, start2, size2);
        if (size1 > 0)
        {
            waveformPeakBuffer   [static_cast<size_t>(start1)] = waveformSlicePeakAccum;
            waveformMinGainBuffer[static_cast<size_t>(start1)] = waveformSliceMinGainAccum;
            waveformFifo.finishedWrite(1);
        }
        waveformSliceSampleCount  = 0;
        waveformSlicePeakAccum    = 0.0f;
        waveformSliceMinGainAccum = 1.0f;
    }
}

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

    // LINK: Threshold と Output Gain を相対オフセットを保ったまま連動させるトグル。
    //  実際の連動ロジックは WebUI 側で、有効時に双方の setNormalisedValue を呼び合う。
    //  DSP には影響しない（状態の持続のみ）。
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        zl::id::LINK,
        "Link",
        false));

    // MODE: Single / Multi バンドモード切替。
    //  Multi はバンド数に応じた LR4 IIR ツリー分割 + バンド毎独立リミッタ。
    //  Multi 時は自動的に Auto Release として振る舞い、手動 RELEASE_MS は無視される。
    //  既定は Multi（ゼロコンフィグで"良い音"が出る方向をデフォルトにする）。
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        zl::id::MODE,
        "Mode",
        juce::StringArray{ "Single", "Multi" },
        1));

    // DISPLAY_MODE: 中央表示のモード（Metering=通常のメーター群 / Waveform=Pro-L 風の波形表示）。
    //  純粋に UI の表示切替で DSP には影響しないが、プリセットや再起動後の状態復帰を
    //  自然に扱うため APVTS で持つ（METERING_MODE と同様）。
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        zl::id::DISPLAY_MODE,
        "Display Mode",
        juce::StringArray{ "Metering", "Waveform" },
        0));

    // BAND_COUNT: Multi モードのバンド数。
    //  3-band: 120 Hz / 5 kHz             （放送、声を Mid に閉じ込め）← 既定
    //  4-band: 150 Hz / 5 kHz / 15 kHz    （Steinberg 準拠）
    //  5-band: 80 / 250 / 1k / 5k Hz      （UA 準拠、音楽マスタリング志向）
    //  既定 3-band：声の一貫性を最優先、最もクセが少なく幅広いソースで破綻しない。
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        zl::id::BAND_COUNT,
        "Band Count",
        juce::StringArray{ "3 Band", "4 Band", "5 Band" },
        0));

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
    multibandLimiter.prepare(sampleRate, getTotalNumOutputChannels(), samplesPerBlock);

    if (auto* p = parameters.getRawParameterValue(zl::id::THRESHOLD.getParamID()))
    {
        limiter.setThresholdDb(p->load());
        multibandLimiter.setThresholdDb(p->load());
    }
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

    // 波形表示用 per-sample gain スクラッチを maxBlockSize で確保（processBlock では追加 alloc しない）
    waveformGainScratchA.assign(static_cast<size_t>(samplesPerBlock), 1.0f);
    waveformGainScratchB.assign(static_cast<size_t>(samplesPerBlock), 1.0f);

    // ---- 波形表示用リングバッファ準備 ----
    //  slice サイズはサンプルレートに応じて決定（約 200 Hz）。
    //  リアロケーションは prepare 時のみに限定する。
    const double sliceHz = kWaveformSliceHz;
    waveformSliceSize = juce::jmax(1, static_cast<int>(std::round(sampleRate / sliceHz)));
    waveformSliceHz.store(static_cast<float>(sampleRate / static_cast<double>(waveformSliceSize)),
                           std::memory_order_relaxed);
    if (static_cast<int>(waveformPeakBuffer.size()) != kWaveformFifoSize)
    {
        waveformPeakBuffer.assign(kWaveformFifoSize, 0.0f);
        waveformMinGainBuffer.assign(kWaveformFifoSize, 1.0f);
    }
    else
    {
        std::fill(waveformPeakBuffer.begin(), waveformPeakBuffer.end(), 0.0f);
        std::fill(waveformMinGainBuffer.begin(), waveformMinGainBuffer.end(), 1.0f);
    }
    waveformFifo.reset();
    waveformSliceSampleCount  = 0;
    waveformSlicePeakAccum    = 0.0f;
    waveformSliceMinGainAccum = 1.0f;
}

void ZeroLimitAudioProcessor::releaseResources()
{
    limiter.reset();
    multibandLimiter.reset();
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

    sanitizeBufferFinite(buffer, numChannels, numSamples);

    // パラメータ取得
    const float thresholdDb = parameters.getRawParameterValue(zl::id::THRESHOLD.getParamID())->load();
    const float outGainDb   = parameters.getRawParameterValue(zl::id::OUTPUT_GAIN.getParamID())->load();
    const float releaseMs   = parameters.getRawParameterValue(zl::id::RELEASE_MS.getParamID())->load();
    const bool  autoRel     = parameters.getRawParameterValue(zl::id::AUTO_RELEASE.getParamID())->load() > 0.5f;
    const bool  multiMode   = parameters.getRawParameterValue(zl::id::MODE.getParamID())->load() > 0.5f;
    const int   bandCountIdx = static_cast<int>(parameters.getRawParameterValue(zl::id::BAND_COUNT.getParamID())->load() + 0.5f);

    // Single 側のリミッタは Multi モードではサム後の最終セーフティとして使う。
    // Multi 時はバンド内リダクションで十分抑えているので、セーフティは位相合成オーバーシュートの
    // 取りこぼしだけを拾えば良い。Auto Release 強制 ON、短めの時定数に設定する。
    limiter.setThresholdDb(thresholdDb);
    if (multiMode)
    {
        limiter.setReleaseMs(5.0f);
        limiter.setSlowReleaseMs(50.0f);
        limiter.setAutoReleaseEnabled(true);

        // バンド数切替。setMode は内部で同じモード指定なら何もしない。
        const auto desiredMode = (bandCountIdx == 0) ? zl::dsp::MultibandLimiter::Mode::Band3
                               : (bandCountIdx == 2) ? zl::dsp::MultibandLimiter::Mode::Band5
                                                     : zl::dsp::MultibandLimiter::Mode::Band4;
        multibandLimiter.setMode(desiredMode);
        multibandLimiter.setThresholdDb(thresholdDb);
    }
    else
    {
        limiter.setReleaseMs(releaseMs);
        limiter.setSlowReleaseMs(150.0f);
        limiter.setAutoReleaseEnabled(autoRel);
    }

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
    //  Single: limiter 1 段のみ
    //  Multi : multibandLimiter（3 バンド）→ limiter（サム後の最終セーフティ）
    //  Pro-L 風のスムーズな GR オーバーレイのため、per-sample gain を scratch バッファに取得して、
    //   multi モードでは multibandLimiter のバンド間最小 × safety limiter の per-sample gain を
    //   要素乗算して "総 gain" として波形 slice に使う。
    float minGain = 1.0f;
    // スクラッチが足りない場合は追加確保（通常は prepare 側で十分。保険として）。
    if (static_cast<int>(waveformGainScratchA.size()) < numSamples)
        waveformGainScratchA.resize(static_cast<size_t>(numSamples), 1.0f);
    if (static_cast<int>(waveformGainScratchB.size()) < numSamples)
        waveformGainScratchB.resize(static_cast<size_t>(numSamples), 1.0f);

    float* gainA = waveformGainScratchA.data();
    float* gainB = waveformGainScratchB.data();

    if (multiMode)
    {
        const float mbGain     = multibandLimiter.processBlock(buffer, gainA);
        const float safetyGain = limiter.processBlock(buffer, gainB);
        minGain = mbGain * safetyGain;
        // per-sample 合成: gainA *= gainB
        for (int i = 0; i < numSamples; ++i)
            gainA[i] *= gainB[i];
    }
    else
    {
        minGain = limiter.processBlock(buffer, gainA);
    }
    atomicMinFloat(minGainAccum, minGain);

    // --- 波形表示用：入力 |L|,|R| マージ済みサンプルを slice にダウンサンプルして FIFO へ ---
    //  Pro-L 風のオシロ表示。pre-limiter 入力ピーク + per-sample gain を渡して slice 内で最小 gain を集計。
    //  これにより DAW ブロックサイズが slice サイズを超えても GR オーバーレイが階段状にならない。
    {
        auto* il = inputCopyBuffer.getReadPointer(0);
        auto* ir = inputCopyBuffer.getReadPointer(std::min(1, numChannels - 1));
        for (int i = 0; i < numSamples; ++i)
        {
            const float mergedAbs = std::max(std::abs(il[i]), std::abs(ir[i]));
            pushWaveformSample(mergedAbs, gainA[i]);
        }
    }

    // --- Auto makeup gain + 出力ゲイン ---
    //  Threshold を下げた分（-thresholdDb）だけリミッタ段後に自動で補償し、
    //  そのうえでユーザー Output Gain を重ねる。
    //  ピーク出力は 10^(outGainDb/20) 相当（= Threshold に依存しない）になる。
    const float makeupDb = -thresholdDb;                     // thresholdDb <= 0 なので makeupDb >= 0
    const float totalLin = std::pow(10.0f, (makeupDb + outGainDb) / 20.0f);
    if (std::abs(totalLin - 1.0f) > 1.0e-6f)
    {
        for (int ch = 0; ch < numChannels; ++ch)
            buffer.applyGain(ch, 0, numSamples, totalLin);
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
