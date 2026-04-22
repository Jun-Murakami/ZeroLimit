#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace zl::id {
    // ゼロレイテンシ・ブリックウォール・リミッター
    // - THRESHOLD: リミット基準となる入力レベル上限（dBFS, -40..0）
    // - OUTPUT_GAIN: リミッター段後の出力ゲイン（dB, -24..+24）
    const juce::ParameterID THRESHOLD{"THRESHOLD", 1};
    const juce::ParameterID OUTPUT_GAIN{"OUTPUT_GAIN", 1};
}  // namespace zl::id
