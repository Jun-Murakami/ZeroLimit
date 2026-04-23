// WASM 用 LR4 IIR クロスオーバー（3 / 4 / 5 バンド可変）。
// プラグイン側 CrossoverLR4 と同じトポロジ・位相アライメントを、純 C++ + 内製 biquad で再実装。
//
// 詳細は plugin/src/dsp/CrossoverLR4.h のコメント参照。
// 出力は band 別の L/R ポインタ配列へ書き出す（ポインタは呼び出し側が numSamples 以上確保）。
#pragma once

#include "biquad.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace zl_wasm {

class CrossoverLR4
{
public:
    static constexpr int   kMaxBands      = 5;
    static constexpr int   kMaxCrossovers = kMaxBands - 1; // 4
    static constexpr int   kMaxChannels   = 2;
    static constexpr double kButterQ      = 0.7071067811865476; // 1/√2

    void prepare(double sr, int nch, int maxBlock) noexcept
    {
        sampleRate    = sr > 0.0 ? sr : 44100.0;
        numChannels   = std::max(1, std::min(kMaxChannels, nch));
        preparedBlock = std::max(1, maxBlock);
        resizeScratch(preparedBlock);
        refreshAll();
        reset();
    }

    void reset() noexcept
    {
        for (auto& p : splitPairs) p.reset();
        for (auto& row : apPairs) for (auto& p : row) p.reset();
    }

    // numBands は 3..5。crossovers は numBands-1 個、昇順。
    void configure(int numBands, const float* crossovers) noexcept
    {
        currentBandCount = std::max(3, std::min(kMaxBands, numBands));

        const double safeU = getSafeUpperFrequency();
        const double baseLower = std::min(10.0, safeU);
        const double minSpacing = std::min(10.0, std::max(0.0, (safeU - baseLower) / static_cast<double>(currentBandCount)));
        double prev = baseLower - minSpacing;
        for (int i = 0; i < currentBandCount - 1; ++i)
        {
            const double lower = std::min(safeU, prev + minSpacing);
            double v = static_cast<double>(crossovers[i]);
            if (v < lower) v = lower;
            if (v > safeU) v = safeU;
            crossoverFreqs[i] = static_cast<float>(v);
            prev = v;
        }
        for (int i = currentBandCount - 1; i < kMaxCrossovers; ++i)
            crossoverFreqs[i] = crossoverFreqs[std::max(0, currentBandCount - 2)];

        refreshAll();
    }

    int getCurrentBandCount() const noexcept { return currentBandCount; }

    // numSamples サンプルを currentBandCount 本のバンドへ分解。
    // bandOutL[i] / bandOutR[i] (i=0..numBands-1) に書き出す。
    // inL / inR は書き換えない。
    void processBlock(const float* inL, const float* inR, int numSamples,
                      float* const* bandOutL, float* const* bandOutR) noexcept
    {
        if (preparedBlock > 0 && numSamples > preparedBlock)
        {
            int offset = 0;
            while (offset < numSamples)
            {
                const int chunk = std::min(preparedBlock, numSamples - offset);
                float* chunkOutL[kMaxBands];
                float* chunkOutR[kMaxBands];
                for (int b = 0; b < kMaxBands; ++b)
                {
                    chunkOutL[b] = bandOutL[b] + offset;
                    chunkOutR[b] = bandOutR[b] + offset;
                }
                processBlock(inL + offset, inR + offset, chunk, chunkOutL, chunkOutR);
                offset += chunk;
            }
            return;
        }

        const int N  = currentBandCount;
        const int nc = numChannels;

        ensureSize(restL, numSamples);
        ensureSize(restR, numSamples);
        ensureSize(rawL,  numSamples);
        ensureSize(rawR,  numSamples);
        ensureSize(tmpAL, numSamples);
        ensureSize(tmpAR, numSamples);
        ensureSize(tmpBL, numSamples);
        ensureSize(tmpBR, numSamples);

        // rest = Input
        std::copy(inL, inL + numSamples, restL.data());
        std::copy(inR, inR + numSamples, restR.data());

        for (int k = 0; k < N - 1; ++k)
        {
            // raw = rest (コピー)
            std::copy(restL.data(), restL.data() + numSamples, rawL.data());
            std::copy(restR.data(), restR.data() + numSamples, rawR.data());

            // raw = LP(raw)  ← B_k の生データ
            splitPairs[k].applyLP(rawL.data(), rawR.data(), numSamples, nc);
            // rest = HP(rest) ← 次段の入力
            splitPairs[k].applyHP(restL.data(), restR.data(), numSamples, nc);

            // B_k に AP 連鎖を適用（c_{k+1}, c_{k+2}, ..., c_{N-2} の AP）
            const int numAp = N - 2 - k;
            for (int j = 0; j < numAp; ++j)
            {
                std::copy(rawL.data(), rawL.data() + numSamples, tmpAL.data());
                std::copy(rawR.data(), rawR.data() + numSamples, tmpAR.data());
                std::copy(rawL.data(), rawL.data() + numSamples, tmpBL.data());
                std::copy(rawR.data(), rawR.data() + numSamples, tmpBR.data());

                apPairs[k][j].applyLP(tmpAL.data(), tmpAR.data(), numSamples, nc);
                apPairs[k][j].applyHP(tmpBL.data(), tmpBR.data(), numSamples, nc);

                for (int i = 0; i < numSamples; ++i)
                {
                    rawL[i] = tmpAL[i] + tmpBL[i];
                    if (nc > 1) rawR[i] = tmpAR[i] + tmpBR[i];
                }
            }

            std::copy(rawL.data(), rawL.data() + numSamples, bandOutL[k]);
            std::copy(rawR.data(), rawR.data() + numSamples, bandOutR[k]);
        }

        // 最後のバンド = rest（AP 不要）
        std::copy(restL.data(), restL.data() + numSamples, bandOutL[N - 1]);
        std::copy(restR.data(), restR.data() + numSamples, bandOutR[N - 1]);
    }

private:
    // LR4 = BW2 の 2 段カスケードで構成する LP/HP ペア。状態は channel 別。
    struct LR4Pair
    {
        Biquad lp[kMaxChannels][2];
        Biquad hp[kMaxChannels][2];

        void setCoefficients(double sr, float freq) noexcept
        {
            Biquad refLp, refHp;
            const float safeFreq = clampFrequencyForCoefficients(sr, freq);
            Biquad::makeLowPass (refLp, sr, safeFreq, kButterQ);
            Biquad::makeHighPass(refHp, sr, safeFreq, kButterQ);
            for (int ch = 0; ch < kMaxChannels; ++ch)
            {
                for (int s = 0; s < 2; ++s)
                {
                    lp[ch][s].b0 = refLp.b0; lp[ch][s].b1 = refLp.b1; lp[ch][s].b2 = refLp.b2;
                    lp[ch][s].a1 = refLp.a1; lp[ch][s].a2 = refLp.a2;
                    hp[ch][s].b0 = refHp.b0; hp[ch][s].b1 = refHp.b1; hp[ch][s].b2 = refHp.b2;
                    hp[ch][s].a1 = refHp.a1; hp[ch][s].a2 = refHp.a2;
                }
            }
        }

        void reset() noexcept
        {
            for (int ch = 0; ch < kMaxChannels; ++ch)
                for (int s = 0; s < 2; ++s) { lp[ch][s].reset(); hp[ch][s].reset(); }
        }

        void applyLP(float* L, float* R, int n, int nc) noexcept
        {
            for (int i = 0; i < n; ++i)
                L[i] = lp[0][1].process(lp[0][0].process(L[i]));
            if (nc > 1)
                for (int i = 0; i < n; ++i)
                    R[i] = lp[1][1].process(lp[1][0].process(R[i]));
        }

        void applyHP(float* L, float* R, int n, int nc) noexcept
        {
            for (int i = 0; i < n; ++i)
                L[i] = hp[0][1].process(hp[0][0].process(L[i]));
            if (nc > 1)
                for (int i = 0; i < n; ++i)
                    R[i] = hp[1][1].process(hp[1][0].process(R[i]));
        }
    };

    static float clampFrequencyForCoefficients(double sr, float freq) noexcept
    {
        const double nyq = (sr > 0.0 ? sr : 44100.0) * 0.5;
        const double upper = std::max(1.0e-3, nyq * 0.95);
        const double lower = std::min(10.0, upper);
        double v = std::isfinite(freq) ? static_cast<double>(freq) : lower;
        if (v < lower) v = lower;
        if (v > upper) v = upper;
        return static_cast<float>(v);
    }

    double getSafeUpperFrequency() const noexcept
    {
        const double nyq = sampleRate * 0.5;
        return std::max(1.0e-3, nyq * 0.95);
    }

    void refreshAll() noexcept
    {
        for (int i = 0; i < kMaxCrossovers; ++i)
            splitPairs[i].setCoefficients(sampleRate, crossoverFreqs[i]);
        // apPairs[b][j] は c_{b+1+j} を表す
        for (int b = 0; b < kMaxBands - 2; ++b)
        {
            for (int j = 0; j < kMaxCrossovers - 1; ++j)
            {
                const int cIdx = b + 1 + j;
                const int useIdx = cIdx < kMaxCrossovers ? cIdx : kMaxCrossovers - 1;
                apPairs[b][j].setCoefficients(sampleRate, crossoverFreqs[useIdx]);
            }
        }
    }

    static void ensureSize(std::vector<float>& v, int n)
    {
        if (static_cast<int>(v.size()) < n) v.resize(static_cast<size_t>(n));
    }

    void resizeScratch(int n)
    {
        restL.resize(static_cast<size_t>(n));
        restR.resize(static_cast<size_t>(n));
        rawL .resize(static_cast<size_t>(n));
        rawR .resize(static_cast<size_t>(n));
        tmpAL.resize(static_cast<size_t>(n));
        tmpAR.resize(static_cast<size_t>(n));
        tmpBL.resize(static_cast<size_t>(n));
        tmpBR.resize(static_cast<size_t>(n));
    }

    double sampleRate  = 44100.0;
    int    numChannels = 2;
    int    preparedBlock = 0;
    int    currentBandCount = 3;
    std::array<float, kMaxCrossovers> crossoverFreqs{ 120.0f, 5000.0f, 15000.0f, 15000.0f };

    std::array<LR4Pair, kMaxCrossovers> splitPairs;
    std::array<std::array<LR4Pair, kMaxCrossovers - 1>, kMaxBands - 2> apPairs;

    std::vector<float> restL, restR, rawL, rawR, tmpAL, tmpAR, tmpBL, tmpBR;
};

} // namespace zl_wasm
