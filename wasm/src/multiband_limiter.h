// WASM 用マルチバンド・リミッター。3/4/5 バンドを動的切替。
// プラグイン側 MultibandLimiter と同じクロスオーバー周波数・時定数を使う。
#pragma once

#include "crossover_lr4.h"
#include "limiter.h"

#include <algorithm>
#include <array>
#include <vector>

namespace zl_wasm {

class MultibandLimiter
{
public:
    static constexpr int kMaxBands = CrossoverLR4::kMaxBands;

    enum Mode { Band3 = 0, Band4 = 1, Band5 = 2 };

    void prepare(double sr, int nch, int maxBlock) noexcept
    {
        sampleRate        = sr;
        preparedChannels  = std::max(1, nch);
        preparedBlock     = std::max(1, maxBlock);

        crossover.prepare(sampleRate, preparedChannels, preparedBlock);

        for (auto& lim : bandLimiters)
            lim.prepare(sampleRate);

        for (int i = 0; i < kMaxBands; ++i)
        {
            bandBufL[i].resize(static_cast<size_t>(preparedBlock));
            bandBufR[i].resize(static_cast<size_t>(preparedBlock));
        }

        configureForMode(currentMode);
    }

    void reset() noexcept
    {
        crossover.reset();
        for (auto& lim : bandLimiters) lim.reset();
    }

    void setThresholdDb(float db) noexcept
    {
        for (auto& lim : bandLimiters) lim.setThresholdDb(db);
    }

    void setMode(Mode m) noexcept
    {
        if (m == currentMode) return;
        currentMode = m;
        configureForMode(m);
        reset();
    }

    Mode getMode() const noexcept { return currentMode; }

    int getNumBands() const noexcept
    {
        switch (currentMode)
        {
            case Band3: return 3;
            case Band4: return 4;
            case Band5: return 5;
        }
        return 3;
    }

    // ステレオ in-place 処理。戻り値はバンド間の最小ゲイン（= 最大リダクション）。
    //  gainOut != nullptr なら各サンプルでの「バンド間最小 gain」（リニア 0..1）を書き出す。
    //  配列長は最低でも numSamples 必要。
    float processStereoInPlace(float* L, float* R, int numSamples, float* gainOut = nullptr) noexcept
    {
        if (preparedBlock > 0 && numSamples > preparedBlock)
        {
            float minG = 1.0f;
            int offset = 0;
            while (offset < numSamples)
            {
                const int chunk = std::min(preparedBlock, numSamples - offset);
                float* gOutChunk = gainOut ? (gainOut + offset) : nullptr;
                const float g = processStereoInPlace(L + offset, R + offset, chunk, gOutChunk);
                if (g < minG) minG = g;
                offset += chunk;
            }
            return minG;
        }

        const int N = getNumBands();

        // prepare 前の直接呼び出しだけに備えた防御。通常経路では事前確保済み。
        for (int i = 0; i < N; ++i)
        {
            if (static_cast<int>(bandBufL[i].size()) < numSamples) bandBufL[i].resize(numSamples);
            if (static_cast<int>(bandBufR[i].size()) < numSamples) bandBufR[i].resize(numSamples);
        }
        if (gainOut && static_cast<int>(bandGainScratch.size()) < numSamples)
            bandGainScratch.resize(static_cast<size_t>(numSamples), 1.0f);

        float* bL[kMaxBands];
        float* bR[kMaxBands];
        for (int i = 0; i < N; ++i) { bL[i] = bandBufL[i].data(); bR[i] = bandBufR[i].data(); }

        crossover.processBlock(L, R, numSamples, bL, bR);

        // gainOut を 1.0 で初期化、バンドごとに per-sample gain の min を取る
        if (gainOut)
            for (int i = 0; i < numSamples; ++i) gainOut[i] = 1.0f;

        float minG = 1.0f;
        for (int i = 0; i < N; ++i)
        {
            float* perSampleOut = gainOut ? bandGainScratch.data() : nullptr;
            const float g = bandLimiters[i].processStereoInPlace(bL[i], bR[i], numSamples, perSampleOut);
            if (g < minG) minG = g;
            if (gainOut)
            {
                for (int s = 0; s < numSamples; ++s)
                    if (perSampleOut[s] < gainOut[s]) gainOut[s] = perSampleOut[s];
            }
        }

        // サム
        for (int i = 0; i < numSamples; ++i)
        {
            float sl = 0.0f, sr = 0.0f;
            for (int b = 0; b < N; ++b) { sl += bL[b][i]; sr += bR[b][i]; }
            L[i] = sl; R[i] = sr;
        }

        return minG;
    }

private:
    // プラグイン側 MultibandLimiter.cpp の kBands3/4/5 / kCrossovers3/4/5 と同じ数値
    struct BandSpec { float fastMs; float slowMs; };

    void configureForMode(Mode m) noexcept
    {
        static const BandSpec kB3[3] = { { 20.0f, 250.0f }, {  5.0f, 150.0f }, { 1.0f, 80.0f } };
        static const BandSpec kB4[4] = { { 20.0f, 250.0f }, {  5.0f, 150.0f }, { 1.0f, 80.0f }, { 0.5f, 50.0f } };
        static const BandSpec kB5[5] = { { 30.0f, 300.0f }, { 15.0f, 200.0f }, { 5.0f, 120.0f }, { 2.0f, 100.0f }, { 1.0f, 80.0f } };
        static const float    kF3[2] = { 120.0f, 5000.0f };
        static const float    kF4[3] = { 150.0f, 5000.0f, 15000.0f };
        static const float    kF5[4] = { 80.0f, 250.0f, 1000.0f, 5000.0f };

        auto apply = [](ZeroLatencyLimiter& lim, const BandSpec& s) {
            lim.setReleaseMs(s.fastMs);
            lim.setSlowReleaseMs(s.slowMs);
            lim.setAutoReleaseEnabled(true);
        };

        if (m == Band3)
        {
            crossover.configure(3, kF3);
            for (int i = 0; i < 3; ++i) apply(bandLimiters[i], kB3[i]);
        }
        else if (m == Band4)
        {
            crossover.configure(4, kF4);
            for (int i = 0; i < 4; ++i) apply(bandLimiters[i], kB4[i]);
        }
        else
        {
            crossover.configure(5, kF5);
            for (int i = 0; i < 5; ++i) apply(bandLimiters[i], kB5[i]);
        }
    }

    double sampleRate       = 44100.0;
    int    preparedChannels = 2;
    int    preparedBlock    = 0;
    Mode   currentMode      = Band3;

    CrossoverLR4 crossover;
    std::array<ZeroLatencyLimiter, kMaxBands> bandLimiters;
    std::array<std::vector<float>, kMaxBands> bandBufL;
    std::array<std::vector<float>, kMaxBands> bandBufR;
    // per-sample gain 集計用スクラッチ（bandLimiter から受け取って min 合成）
    std::vector<float> bandGainScratch;
};

} // namespace zl_wasm
