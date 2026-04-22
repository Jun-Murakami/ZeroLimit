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

        crossover.prepare(sampleRate, preparedChannels);

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
    float processStereoInPlace(float* L, float* R, int numSamples) noexcept
    {
        const int N = getNumBands();

        // バッファが足りなければ再確保（WASM 内では stop-the-world 的な確保だが稀）
        for (int i = 0; i < N; ++i)
        {
            if (static_cast<int>(bandBufL[i].size()) < numSamples) bandBufL[i].resize(numSamples);
            if (static_cast<int>(bandBufR[i].size()) < numSamples) bandBufR[i].resize(numSamples);
        }

        float* bL[kMaxBands];
        float* bR[kMaxBands];
        for (int i = 0; i < N; ++i) { bL[i] = bandBufL[i].data(); bR[i] = bandBufR[i].data(); }

        crossover.processBlock(L, R, numSamples, bL, bR);

        float minG = 1.0f;
        for (int i = 0; i < N; ++i)
        {
            const float g = bandLimiters[i].processStereoInPlace(bL[i], bR[i], numSamples);
            if (g < minG) minG = g;
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
};

} // namespace zl_wasm
