#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace zl::id {
    // ゼロレイテンシ・ブリックウォール・リミッター
    // - THRESHOLD: リミット基準レベル（dBFS, -30..0）
    // - OUTPUT_GAIN: リミッター段後の出力トリム（dB, -30..0）
    // - METERING_MODE: メーター表示モード（0=Peak / 1=RMS / 2=Momentary）
    // - RELEASE_MS: 手動リリース時定数（0.01..1000 ms、log skew、既定 1.0）
    // - AUTO_RELEASE: プログラム依存リリース（既定 ON）
    // - LINK: Threshold と Output Gain を相対オフセット固定で連動させる（既定 OFF）
    // - MODE: Single / Multi バンドモード切替（0=Single, 1=Multi、既定 Single）
    //         Multi 時は AUTO_RELEASE を強制 ON として扱い、手動 RELEASE_MS は無視される。
    // - BAND_COUNT: Multi モード時のバンド数（0=3 band, 1=4 band, 2=5 band、既定 4 band）
    const juce::ParameterID THRESHOLD{"THRESHOLD", 1};
    const juce::ParameterID OUTPUT_GAIN{"OUTPUT_GAIN", 1};
    const juce::ParameterID METERING_MODE{"METERING_MODE", 1};
    const juce::ParameterID RELEASE_MS{"RELEASE_MS", 1};
    const juce::ParameterID AUTO_RELEASE{"AUTO_RELEASE", 1};
    const juce::ParameterID LINK{"LINK", 1};
    const juce::ParameterID MODE{"MODE", 1};
    const juce::ParameterID BAND_COUNT{"BAND_COUNT", 1};
}  // namespace zl::id
