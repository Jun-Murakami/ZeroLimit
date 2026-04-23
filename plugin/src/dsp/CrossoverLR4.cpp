#include "CrossoverLR4.h"
#include <algorithm>

namespace zl::dsp {

void CrossoverLR4::prepare(double sr, int channelsIn, int maximumBlockSize)
{
    sampleRate  = sr > 0.0 ? sr : 44100.0;
    numChannels = std::min(kMaxChannels, std::max(1, channelsIn));
    preparedBlockSize = std::max(1, maximumBlockSize);

    juce::dsp::ProcessSpec spec{};
    spec.sampleRate       = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32>(preparedBlockSize);
    spec.numChannels      = 1;

    for (auto& p : splitPairs) p.prepare(spec);
    for (auto& row : apPairs)
        for (auto& p : row) p.prepare(spec);

    updateCoefficients();
    assignCoefficientsToAllPairs();

    auto prepareBuffer = [this](juce::AudioBuffer<float>& b)
    {
        b.setSize(numChannels,
                  preparedBlockSize,
                  /*keepExistingContent*/ false,
                  /*clearExtraSpace*/     true,
                  /*avoidReallocating*/   false);
    };
    prepareBuffer(restBuf);
    prepareBuffer(bandRawBuf);
    prepareBuffer(tmpA);
    prepareBuffer(tmpB);

    reset();
}

void CrossoverLR4::reset()
{
    for (auto& p : splitPairs) p.reset();
    for (auto& row : apPairs)
        for (auto& p : row) p.reset();
}

void CrossoverLR4::configure(int numBands, const float* crossovers)
{
    const int n = std::clamp(numBands, 3, kMaxBands);
    currentBandCount = n;

    const float nyq   = static_cast<float>(sampleRate) * 0.5f;
    const float safeU = std::max(0.001f, nyq * 0.95f);

    // 昇順に clamp しながら格納。低サンプルレートで safeU が狭い場合は間隔を縮める。
    const int numCrossovers = n - 1;
    const float baseMin = std::min(10.0f, safeU);
    const float available = std::max(0.0f, safeU - baseMin);
    const float minSpacing = numCrossovers > 1
                               ? std::min(10.0f, available / static_cast<float>(numCrossovers - 1))
                               : 0.0f;
    float prev = baseMin;
    for (int i = 0; i < n - 1; ++i)
    {
        const float remaining = static_cast<float>(numCrossovers - 1 - i);
        const float upper = safeU - remaining * minSpacing;
        const float lower = (i == 0) ? std::min(baseMin, upper)
                                     : std::min(prev + minSpacing, upper);
        float v = std::max(lower, std::min(crossovers[i], upper));
        crossoverFreqs[i] = v;
        prev = v;
    }
    // 未使用スロットはクリア（安全のため、最後の値を踏襲）
    for (int i = n - 1; i < kMaxCrossovers; ++i)
        crossoverFreqs[i] = crossoverFreqs[std::max(0, n - 2)];

    updateCoefficients();
    assignCoefficientsToAllPairs();
}

void CrossoverLR4::updateCoefficients()
{
    for (int i = 0; i < kMaxCrossovers; ++i)
    {
        const float nyq = static_cast<float>(sampleRate) * 0.5f;
        const float upper = std::max(0.001f, nyq * 0.95f);
        const float lower = std::min(10.0f, upper);
        const float f = std::max(lower, std::min(crossoverFreqs[i], upper));
        lpCoefs[i] = Coefs::makeLowPass (sampleRate, f, kButterQ);
        hpCoefs[i] = Coefs::makeHighPass(sampleRate, f, kButterQ);
    }
}

void CrossoverLR4::assignCoefficientsToAllPairs()
{
    // splitPairs[k] は c_k で分割
    for (int k = 0; k < kMaxCrossovers; ++k)
        splitPairs[k].setCoefficients(lpCoefs[k], hpCoefs[k]);

    // apPairs[b][j] は c_{b+1+j} で AP
    for (int b = 0; b < kMaxBands - 2; ++b)
    {
        for (int j = 0; j < kMaxCrossovers - 1; ++j)
        {
            const int cIdx = b + 1 + j;
            if (cIdx < kMaxCrossovers)
                apPairs[b][j].setCoefficients(lpCoefs[cIdx], hpCoefs[cIdx]);
            else
                // 未使用スロット（その band では到達しない）。何か割り当てておく。
                apPairs[b][j].setCoefficients(lpCoefs[kMaxCrossovers - 1], hpCoefs[kMaxCrossovers - 1]);
        }
    }
}

void CrossoverLR4::processBlock(const juce::AudioBuffer<float>& input,
                                std::array<juce::AudioBuffer<float>, kMaxBands>& bandOuts) noexcept
{
    const int channels = std::min(numChannels, input.getNumChannels());
    const int n        = input.getNumSamples();
    if (channels <= 0 || n <= 0)
        return;

    const int N = currentBandCount;
    const int numCrossovers = N - 1;

    auto ensure = [&](juce::AudioBuffer<float>& b)
    {
        if (b.getNumChannels() != channels || b.getNumSamples() != n)
            b.setSize(channels, n, false, false, true);
    };
    ensure(restBuf);
    ensure(bandRawBuf);
    ensure(tmpA);
    ensure(tmpB);
    for (int i = 0; i < N; ++i)
        ensure(bandOuts[i]);

    // restBuf ← Input
    for (int ch = 0; ch < channels; ++ch)
        restBuf.copyFrom(ch, 0, input, ch, 0, n);

    // 分割を N-1 段繰り返す。
    //   stage k:
    //     bandRawBuf = LP_LR4@c_k(restBuf)   ← これが B_k の「生」データ
    //     restBuf    = HP_LR4@c_k(restBuf)   ← 残りの信号。次の段へ。
    //
    //   各 B_k に対して AP 連鎖（AP@c_{k+1}, ..., AP@c_{N-2}）を適用して
    //   位相アライン済みの band を bandOuts[k] に書き出す。
    for (int k = 0; k < numCrossovers; ++k)
    {
        // bandRawBuf ← restBuf (コピー)
        for (int ch = 0; ch < channels; ++ch)
            bandRawBuf.copyFrom(ch, 0, restBuf, ch, 0, n);

        // bandRawBuf に LP を適用（これが B_k の生データ）
        splitPairs[k].applyLP(bandRawBuf, channels, n);

        // restBuf に HP を適用（これが rest_k。次段の入力）
        splitPairs[k].applyHP(restBuf, channels, n);

        // B_k の AP 連鎖適用。Need AP at c_{k+1}, c_{k+2}, ..., c_{numCrossovers-1}
        // apPairs[k][j] は c_{k+1+j} を表すので、j = 0..(numCrossovers-1 - (k+1)) = numCrossovers-2-k
        const int numApStages = numCrossovers - 1 - k;
        if (numApStages <= 0)
        {
            // AP 不要（N-2, N-1 バンドのうちの前者）: そのままコピー
            for (int ch = 0; ch < channels; ++ch)
                bandOuts[k].copyFrom(ch, 0, bandRawBuf, ch, 0, n);
        }
        else
        {
            // AP チェーンを順に適用。結果は bandRawBuf に書き戻しながら進める。
            for (int j = 0; j < numApStages; ++j)
            {
                // tmpA = LP(bandRawBuf), tmpB = HP(bandRawBuf)
                for (int ch = 0; ch < channels; ++ch)
                {
                    tmpA.copyFrom(ch, 0, bandRawBuf, ch, 0, n);
                    tmpB.copyFrom(ch, 0, bandRawBuf, ch, 0, n);
                }
                apPairs[k][j].applyLP(tmpA, channels, n);
                apPairs[k][j].applyHP(tmpB, channels, n);
                // bandRawBuf = tmpA + tmpB（AP の出力）
                for (int ch = 0; ch < channels; ++ch)
                {
                    auto*       out = bandRawBuf.getWritePointer(ch);
                    const auto* lp  = tmpA.getReadPointer(ch);
                    const auto* hp  = tmpB.getReadPointer(ch);
                    for (int i = 0; i < n; ++i)
                        out[i] = lp[i] + hp[i];
                }
            }
            // bandOuts[k] ← bandRawBuf（AP 済み）
            for (int ch = 0; ch < channels; ++ch)
                bandOuts[k].copyFrom(ch, 0, bandRawBuf, ch, 0, n);
        }
    }

    // 最後のバンド B_{N-1} は restBuf をそのまま使う（N-1 段分の HP を通過済み、AP 不要）
    for (int ch = 0; ch < channels; ++ch)
        bandOuts[N - 1].copyFrom(ch, 0, restBuf, ch, 0, n);
}

} // namespace zl::dsp
