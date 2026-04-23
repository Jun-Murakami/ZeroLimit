// WASM デモ用 DSP オーケストレータ。
// - 1 本のオーディオソースを保持（PCM L/R）
// - トランスポート（再生 / 停止 / シーク / ループ）
// - 必要に応じて簡易リサンプル（Web Audio サンプルレートと入力サンプルレートが異なる場合）
// - シングル / マルチバンド（3/4/5）リミッタ切替
// - Input / GR / Output メーター（Peak / RMS / Momentary）をブロックで集計して取り出せる
#pragma once

#include "limiter.h"
#include "multiband_limiter.h"
#include "momentary_processor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace zl_wasm {

class DspEngine
{
public:
    // 波形表示用スライスレート（Pro-L 風のオシロ表示用。プラグイン版と揃える）
    static constexpr double kWaveformSliceHz = 200.0;
    static constexpr int    kWaveformRingSize = 2048; // ~10 秒ぶん（200 Hz × 10）

    void prepare(double sr, int maxBlock) noexcept
    {
        sampleRate = sr > 0.0 ? sr : 48000.0;
        maxBlockSize = std::max(1, maxBlock);

        singleLimiter.prepare(sampleRate);
        singleLimiter.setAutoReleaseEnabled(true);
        singleLimiter.setReleaseMs(1.0f);
        singleLimiter.setSlowReleaseMs(150.0f);

        multiLimiter.prepare(sampleRate, 2, maxBlockSize);

        safetyLimiter.prepare(sampleRate);
        safetyLimiter.setAutoReleaseEnabled(true);
        safetyLimiter.setReleaseMs(5.0f);
        safetyLimiter.setSlowReleaseMs(50.0f);

        momentaryIn.prepare (sampleRate, maxBlockSize);
        momentaryOut.prepare(sampleRate, maxBlockSize);

        // 作業用スクラッチ
        scratchInL .resize(static_cast<size_t>(maxBlockSize));
        scratchInR .resize(static_cast<size_t>(maxBlockSize));

        // 波形表示用 per-sample gain スクラッチ（multibandLimiter と safetyLimiter の per-sample gain を保持）
        waveformGainScratchA.assign(static_cast<size_t>(maxBlockSize), 1.0f);
        waveformGainScratchB.assign(static_cast<size_t>(maxBlockSize), 1.0f);

        // 波形スライス：約 200 Hz にダウンサンプル。リングは 2048 スロット（~10 秒）。
        waveformSliceSize = std::max(1, static_cast<int>(std::round(sampleRate / kWaveformSliceHz)));
        waveformPeaks.assign(static_cast<size_t>(kWaveformRingSize), 0.0f);
        waveformGrDb .assign(static_cast<size_t>(kWaveformRingSize), 0.0f);
        waveformWriteIdx = 0;
        waveformReadIdx  = 0;
        waveformSliceSampleCount = 0;
        waveformSlicePeakAccum   = 0.0f;
        waveformSliceGrAccum     = 0.0f;

        resetMeters();
    }

    // ====== ソース管理 ======

    // JS から渡された PCM をエンジン側にコピーして保持。
    //  sourceSampleRate が sampleRate と異なる場合、線形補間リサンプルを内部で使う。
    void loadSource(const float* L, const float* R, int numSamples, double sourceSampleRate) noexcept
    {
        if (numSamples <= 0) { clearSource(); return; }
        sourceL.assign(L, L + numSamples);
        sourceR.assign(R ? R : L, (R ? R : L) + numSamples); // R が null ならモノ複製
        for (auto& s : sourceL) s = sanitizeFinite(s);
        for (auto& s : sourceR) s = sanitizeFinite(s);
        sourceNumSamples = numSamples;
        sourceRate       = sourceSampleRate > 0.0 ? sourceSampleRate : sampleRate;

        rateRatio = sourceRate / sampleRate;
        playPos   = 0.0;
        playing   = false;
        stoppedAtEnd = false;
    }

    void clearSource() noexcept
    {
        sourceL.clear(); sourceR.clear();
        sourceNumSamples = 0;
        playPos = 0.0;
        playing = false;
    }

    bool hasSource() const noexcept { return sourceNumSamples > 0; }

    // ====== トランスポート ======

    void setPlaying(bool p) noexcept
    {
        if (p && !hasSource()) return;
        // 曲末で停止していた状態から再生すると先頭から始まるように
        if (p && stoppedAtEnd) { playPos = 0.0; stoppedAtEnd = false; }
        playing = p;
    }

    bool isPlaying() const noexcept { return playing; }

    void setLoop(bool enabled) noexcept { loopEnabled = enabled; }
    bool getLoop() const noexcept { return loopEnabled; }

    // 0..1 normalised 位置で seek
    void seekNormalized(double norm) noexcept
    {
        if (sourceNumSamples <= 0) return;
        if (norm < 0.0) norm = 0.0;
        if (norm > 1.0) norm = 1.0;
        playPos = norm * static_cast<double>(sourceNumSamples);
        stoppedAtEnd = false;
    }

    double getPositionSeconds() const noexcept
    {
        if (sourceRate <= 0.0) return 0.0;
        return playPos / sourceRate;
    }

    double getDurationSeconds() const noexcept
    {
        if (sourceRate <= 0.0) return 0.0;
        return static_cast<double>(sourceNumSamples) / sourceRate;
    }

    bool consumeStoppedAtEnd() noexcept
    {
        if (stoppedAtEnd) { stoppedAtEnd = false; return true; }
        return false;
    }

    // ====== パラメータ ======

    void setThresholdDb(float db) noexcept
    {
        thresholdDb = clampDb(db);
        singleLimiter.setThresholdDb(thresholdDb);
        multiLimiter.setThresholdDb(thresholdDb);
        safetyLimiter.setThresholdDb(thresholdDb);
    }

    void setOutputGainDb(float db) noexcept { outputGainDb = clampDb(db); }

    void setReleaseMs(float ms) noexcept
    {
        releaseMs = std::isfinite(ms) ? std::max(0.01f, std::min(1000.0f, ms)) : 1.0f;
        singleLimiter.setReleaseMs(releaseMs);
    }

    void setAutoRelease(bool enabled) noexcept
    {
        autoRelease = enabled;
        singleLimiter.setAutoReleaseEnabled(enabled);
    }

    void setMode(int multi) noexcept { multiMode = (multi != 0); }

    void setBandCount(int n) noexcept
    {
        if (n <= 3) multiLimiter.setMode(MultibandLimiter::Band3);
        else if (n == 4) multiLimiter.setMode(MultibandLimiter::Band4);
        else multiLimiter.setMode(MultibandLimiter::Band5);
    }

    void setMeteringMode(int mode) noexcept { meteringMode = mode; }

    void setBypass(bool b) noexcept { bypass = b; }

    // ====== メイン処理 ======

    // outL / outR を numSamples サンプル分書き出す。
    // 再生中でない場合はゼロ詰め（無音）。
    void processBlock(float* outL, float* outR, int numSamples) noexcept
    {
        if (numSamples <= 0) return;
        if (numSamples > maxBlockSize)
        {
            // 想定外の大きいブロックには分割再帰（WASM 処理は 128 固定が標準）
            int offset = 0;
            while (offset < numSamples)
            {
                const int chunk = std::min(maxBlockSize, numSamples - offset);
                processBlock(outL + offset, outR + offset, chunk);
                offset += chunk;
            }
            return;
        }

        // --- 1) ソースから PCM を取り出す（再生中でなければゼロ） ---
        fetchSource(outL, outR, numSamples);
        sanitizeStereo(outL, outR, numSamples);

        // 入力側の生サンプルを波形表示用に控えておく（limiter で破壊される前）
        for (int i = 0; i < numSamples; ++i)
        {
            scratchInL[static_cast<size_t>(i)] = outL[i];
            scratchInR[static_cast<size_t>(i)] = outR[i];
        }

        // --- 2) 入力側メーター蓄積 ---
        accumInMeters(outL, outR, numSamples);
        momentaryIn.processStereo(outL, outR, numSamples);

        // Bypass のときはここで終了（ただし Output ゲインも適用しない）
        if (bypass)
        {
            accumOutMeters(outL, outR, numSamples);
            momentaryOut.processStereo(outL, outR, numSamples);
            return;
        }

        // --- 3) リミッタ ---
        //  Pro-L 風スムーズ GR オーバーレイ用に per-sample gain を scratch A/B に取得。
        //  multi モード: バンド間最小(A) × safety(B) で per-sample total gain を作る。
        //  single モード: limiter の per-sample gain を直接使う。
        if (static_cast<int>(waveformGainScratchA.size()) < numSamples)
            waveformGainScratchA.resize(static_cast<size_t>(numSamples), 1.0f);
        if (static_cast<int>(waveformGainScratchB.size()) < numSamples)
            waveformGainScratchB.resize(static_cast<size_t>(numSamples), 1.0f);
        float* gainA = waveformGainScratchA.data();
        float* gainB = waveformGainScratchB.data();

        float minGain = 1.0f;
        if (multiMode)
        {
            const float mb   = multiLimiter.processStereoInPlace(outL, outR, numSamples, gainA);
            const float safe = safetyLimiter.processStereoInPlace(outL, outR, numSamples, gainB);
            minGain = mb * safe;
            for (int i = 0; i < numSamples; ++i) gainA[i] *= gainB[i];
        }
        else
        {
            minGain = singleLimiter.processStereoInPlace(outL, outR, numSamples, gainA);
        }

        // --- 4) Auto Makeup + Output Gain ---
        const float makeupDb = -thresholdDb;
        const float total    = std::pow(10.0f, (makeupDb + outputGainDb) / 20.0f);
        if (std::fabs(total - 1.0f) > 1.0e-6f)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                outL[i] *= total;
                outR[i] *= total;
            }
        }

        // --- 5) 出力側メーター蓄積 ---
        accumOutMeters(outL, outR, numSamples);
        momentaryOut.processStereo(outL, outR, numSamples);

        // GR を dB で更新（区間最大リダクション）
        const float grDb = (minGain > 0.0f && minGain < 1.0f)
            ? -20.0f * std::log10(minGain) : 0.0f;
        if (grDb > grDbAccum) grDbAccum = grDb;

        // --- 6) 波形表示用スライス蓄積 ---
        //  入力（pre-limiter）の |L|,|R| マージ済みピークと per-sample gain（gainA）を slice に積む。
        accumWaveformSlices(scratchInL.data(), scratchInR.data(), numSamples, gainA);
    }

    // ====== メーターデータ取り出し ======
    // レイアウト: [0]=mode, [1..3]=in (peakL, peakR, ignored),
    //             [4..6]=rms(inL, inR, momentary),
    //             [7..9]=out (peakL, peakR, momentary),
    //             [10]=grDb,
    //             [11..13]=inRmsL, inRmsR, outRmsL,
    //             [14..16]=outRmsR, inMomentary, outMomentary
    // …要件に応じて順序は拡張可能。下の定義を合わせれば JS で読みやすい。
    //
    // 実装では下記の 13 要素で統一:
    //   0: mode
    //   1: inPeakL  2: inPeakR  3: inRmsL  4: inRmsR  5: inMomentary
    //   6: outPeakL 7: outPeakR 8: outRmsL 9: outRmsR 10: outMomentary
    //   11: grDb
    //   12: reserved
    void getMeterData(float* out) noexcept
    {
        const float minDb   = -60.0f;
        const float minLkfs = -70.0f;

        out[0]  = static_cast<float>(meteringMode);
        out[1]  = amplitudeToDb(inPeakAccumL, minDb);
        out[2]  = amplitudeToDb(inPeakAccumR, minDb);
        out[3]  = amplitudeToDb(inRmsAccumL,  minDb);
        out[4]  = amplitudeToDb(inRmsAccumR,  minDb);
        out[5]  = momentaryIn.getMomentaryLKFS();
        if (out[5] < minLkfs) out[5] = minLkfs;
        out[6]  = amplitudeToDb(outPeakAccumL, minDb);
        out[7]  = amplitudeToDb(outPeakAccumR, minDb);
        out[8]  = amplitudeToDb(outRmsAccumL,  minDb);
        out[9]  = amplitudeToDb(outRmsAccumR,  minDb);
        out[10] = momentaryOut.getMomentaryLKFS();
        if (out[10] < minLkfs) out[10] = minLkfs;
        out[11] = grDbAccum;
        out[12] = 0.0f;

        // プラグイン版 PluginEditor::timerCallback と同じ "アタック瞬時 / リリース指数減衰" を
        // ここで適用する。processBlock 側は max 更新（accumInMeters/accumOutMeters 参照）しか
        // しないので、取り出し後に係数を掛けてやることで次フレームの floor が指数減衰する。
        // plugin は 30Hz × 0.93（≒ 19 dB/sec）。こちらは 20Hz なので同等の見た目に揃えるため
        // 0.89（= 0.93^1.5 ≈ -1 dB/frame、-20 dB/sec 相当）を使う。
        //   GR は 1.0 側（= リダクション 0）に戻す方向なので grDbAccum *= decay でよい。
        constexpr float kMeterDecay = 0.89f;
        inPeakAccumL  *= kMeterDecay; inPeakAccumR  *= kMeterDecay;
        outPeakAccumL *= kMeterDecay; outPeakAccumR *= kMeterDecay;
        inRmsAccumL   *= kMeterDecay; inRmsAccumR   *= kMeterDecay;
        outRmsAccumL  *= kMeterDecay; outRmsAccumR  *= kMeterDecay;
        grDbAccum     *= kMeterDecay;
    }

    void resetMomentaryHold() noexcept
    {
        momentaryIn.reset();
        momentaryOut.reset();
    }

    // ====== 波形スライス取り出し ======
    //  リングバッファから max N slice を取り出して peaks[] と grDb[] に書き込む。
    //  戻り値: 実際に取り出した slice 数。
    int getWaveformSlices(float* peaks, float* grDb, int maxN) noexcept
    {
        if (maxN <= 0 || !peaks || !grDb) return 0;
        int count = 0;
        while (count < maxN && waveformReadIdx != waveformWriteIdx)
        {
            peaks[count] = waveformPeaks[static_cast<size_t>(waveformReadIdx)];
            grDb [count] = waveformGrDb [static_cast<size_t>(waveformReadIdx)];
            waveformReadIdx = (waveformReadIdx + 1) % kWaveformRingSize;
            ++count;
        }
        return count;
    }

    // slice レート（ダウンサンプル後のレート）。JS 側の表示スケールに使う。
    double getWaveformSliceHz() const noexcept
    {
        return sampleRate / static_cast<double>(std::max(1, waveformSliceSize));
    }

private:
    static float amplitudeToDb(float amp, float floorDb) noexcept
    {
        if (! std::isfinite(amp) || amp <= 0.0f) return floorDb;
        const float db = 20.0f * std::log10(amp);
        return std::max(db, floorDb);
    }

    static float sanitizeFinite(float v) noexcept
    {
        return std::isfinite(v) ? v : 0.0f;
    }

    static float clampDb(float db) noexcept
    {
        if (! std::isfinite(db)) return 0.0f;
        return std::max(-30.0f, std::min(0.0f, db));
    }

    static void sanitizeStereo(float* L, float* R, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            L[i] = sanitizeFinite(L[i]);
            R[i] = sanitizeFinite(R[i]);
        }
    }

    // 入力バッファに現在の再生位置から PCM をコピー。再生中でなければゼロ。
    // sourceRate != sampleRate のときは線形補間リサンプル。
    void fetchSource(float* outL, float* outR, int n) noexcept
    {
        if (!playing || sourceNumSamples <= 0)
        {
            std::memset(outL, 0, sizeof(float) * static_cast<size_t>(n));
            std::memset(outR, 0, sizeof(float) * static_cast<size_t>(n));
            return;
        }

        for (int i = 0; i < n; ++i)
        {
            double idx = playPos;
            int i0 = static_cast<int>(idx);
            int i1 = i0 + 1;
            double frac = idx - static_cast<double>(i0);

            if (i0 >= sourceNumSamples)
            {
                // 曲末
                if (loopEnabled)
                {
                    playPos = 0.0; stoppedAtEnd = false;
                    idx = 0.0; i0 = 0; i1 = 1; frac = 0.0;
                }
                else
                {
                    outL[i] = 0.0f; outR[i] = 0.0f;
                    playing = false;
                    stoppedAtEnd = true;
                    // 残りのサンプルをゼロ埋めして抜ける
                    for (int k = i + 1; k < n; ++k) { outL[k] = 0.0f; outR[k] = 0.0f; }
                    return;
                }
            }
            if (i1 >= sourceNumSamples)
            {
                i1 = loopEnabled ? 0 : i0;
            }

            const float l0 = sourceL[static_cast<size_t>(i0)];
            const float l1 = sourceL[static_cast<size_t>(i1)];
            const float r0 = sourceR[static_cast<size_t>(i0)];
            const float r1 = sourceR[static_cast<size_t>(i1)];
            outL[i] = static_cast<float>(l0 + (l1 - l0) * frac);
            outR[i] = static_cast<float>(r0 + (r1 - r0) * frac);

            playPos += rateRatio;
        }
    }

    void accumInMeters(const float* L, const float* R, int n) noexcept
    {
        float pL = inPeakAccumL, pR = inPeakAccumR;
        double sumL = 0.0, sumR = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const float aL = std::fabs(L[i]);
            const float aR = std::fabs(R[i]);
            if (aL > pL) pL = aL;
            if (aR > pR) pR = aR;
            sumL += static_cast<double>(L[i]) * L[i];
            sumR += static_cast<double>(R[i]) * R[i];
        }
        inPeakAccumL = pL;
        inPeakAccumR = pR;
        const float rmsL = static_cast<float>(std::sqrt(sumL / static_cast<double>(n)));
        const float rmsR = static_cast<float>(std::sqrt(sumR / static_cast<double>(n)));
        if (rmsL > inRmsAccumL) inRmsAccumL = rmsL;
        if (rmsR > inRmsAccumR) inRmsAccumR = rmsR;
    }

    void accumOutMeters(const float* L, const float* R, int n) noexcept
    {
        float pL = outPeakAccumL, pR = outPeakAccumR;
        double sumL = 0.0, sumR = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const float aL = std::fabs(L[i]);
            const float aR = std::fabs(R[i]);
            if (aL > pL) pL = aL;
            if (aR > pR) pR = aR;
            sumL += static_cast<double>(L[i]) * L[i];
            sumR += static_cast<double>(R[i]) * R[i];
        }
        outPeakAccumL = pL;
        outPeakAccumR = pR;
        const float rmsL = static_cast<float>(std::sqrt(sumL / static_cast<double>(n)));
        const float rmsR = static_cast<float>(std::sqrt(sumR / static_cast<double>(n)));
        if (rmsL > outRmsAccumL) outRmsAccumL = rmsL;
        if (rmsR > outRmsAccumR) outRmsAccumR = rmsR;
    }

    void resetMeters() noexcept
    {
        inPeakAccumL = inPeakAccumR = 0.0f;
        outPeakAccumL = outPeakAccumR = 0.0f;
        inRmsAccumL = inRmsAccumR = 0.0f;
        outRmsAccumL = outRmsAccumR = 0.0f;
        grDbAccum = 0.0f;
    }

    // 入力サンプルを slice にダウンサンプリングして内部リングに push。
    //  - 1 slice = waveformSliceSize 個のサンプルぶんの:
    //      peak     = max(|L|,|R|) の最大値（pre-limiter）
    //      grDb     = per-sample gain から算出した最大リダクション（dB）
    //  - per-sample gain はリミッタから直接取得（出力/入力比ではなく、適用された gain そのもの）。
    //  - リング満杯時は最古を 1 つ捨てる（古いデータを優先的に失う）。
    void accumWaveformSlices(const float* L, const float* R, int n, const float* perSampleGain) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const float mergedAbs = std::max(std::fabs(L[i]), std::fabs(R[i]));
            if (mergedAbs > waveformSlicePeakAccum) waveformSlicePeakAccum = mergedAbs;

            // per-sample gain → dB に変換し、slice 内最大リダクション（= 最小 gain）を追跡
            const float gLin = perSampleGain[i];
            const float perSampleGrDb = (gLin >= 1.0f)
                ? 0.0f
                : -20.0f * std::log10(std::max(gLin, 1.0e-6f));
            if (perSampleGrDb > waveformSliceGrAccum) waveformSliceGrAccum = perSampleGrDb;

            if (++waveformSliceSampleCount >= waveformSliceSize)
            {
                const int nextWrite = (waveformWriteIdx + 1) % kWaveformRingSize;
                if (nextWrite == waveformReadIdx)
                {
                    // 満杯 → 最古を 1 つ捨てる（JS が取り損ねた分は失われる）
                    waveformReadIdx = (waveformReadIdx + 1) % kWaveformRingSize;
                }
                waveformPeaks[static_cast<size_t>(waveformWriteIdx)] = waveformSlicePeakAccum;
                waveformGrDb [static_cast<size_t>(waveformWriteIdx)] = waveformSliceGrAccum;
                waveformWriteIdx = nextWrite;

                waveformSliceSampleCount = 0;
                waveformSlicePeakAccum   = 0.0f;
                waveformSliceGrAccum     = 0.0f;
            }
        }
    }

    // ---- state ----
    double sampleRate = 48000.0;
    int    maxBlockSize = 128;

    // Source
    std::vector<float> sourceL, sourceR;
    int    sourceNumSamples = 0;
    double sourceRate = 48000.0;
    double rateRatio = 1.0;

    // Transport
    double playPos = 0.0;
    bool   playing = false;
    bool   loopEnabled = true;
    bool   stoppedAtEnd = false;

    // Params
    float  thresholdDb  = 0.0f;
    float  outputGainDb = 0.0f;
    float  releaseMs    = 1.0f;
    bool   autoRelease  = true;
    bool   multiMode    = true;
    int    meteringMode = 0;
    bool   bypass       = false;

    // DSP
    ZeroLatencyLimiter singleLimiter;
    MultibandLimiter   multiLimiter;
    ZeroLatencyLimiter safetyLimiter;
    MomentaryProcessor momentaryIn;
    MomentaryProcessor momentaryOut;

    // Scratch
    std::vector<float> scratchInL, scratchInR;
    // 波形表示用 per-sample gain スクラッチ（multi 時は A=バンド間最小、B=safety）
    std::vector<float> waveformGainScratchA;
    std::vector<float> waveformGainScratchB;

    // Meter accumulators（amplitude で保持、取り出し時に dB 変換）
    float inPeakAccumL  = 0.0f, inPeakAccumR  = 0.0f;
    float outPeakAccumL = 0.0f, outPeakAccumR = 0.0f;
    float inRmsAccumL   = 0.0f, inRmsAccumR   = 0.0f;
    float outRmsAccumL  = 0.0f, outRmsAccumR  = 0.0f;
    float grDbAccum     = 0.0f;

    // Waveform ring（audio thread が write、JS が getWaveformSlices で read）
    std::vector<float> waveformPeaks;
    std::vector<float> waveformGrDb;
    int   waveformWriteIdx = 0;
    int   waveformReadIdx  = 0;
    int   waveformSliceSize = 240;        // prepare() で sampleRate から計算
    int   waveformSliceSampleCount = 0;
    float waveformSlicePeakAccum = 0.0f;
    float waveformSliceGrAccum   = 0.0f;
};

} // namespace zl_wasm
