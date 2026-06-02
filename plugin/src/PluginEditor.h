// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginProcessor.h"
#include <optional>

class ZeroLimitAudioProcessorEditor : public juce::AudioProcessorEditor,
                                      private juce::Timer
{
public:
    // ウィンドウの最小・最大サイズ。全てのリサイズ経路（OS 四辺・内蔵リサイザ・
    // WebUI オーバーレイ）で同じ値を使う。
    // 最小 340×400: ノートPC のような小画面でも収まるサイズ。これ以下では各メーターが
    // つぶれて読み取れなくなるため、UI 側で個別クランプを置かず本値で一括制限する。
    static constexpr int kMinWidth  = 340;
    static constexpr int kMinHeight = 400;
    static constexpr int kMaxWidth  = 2560;
    static constexpr int kMaxHeight = 1440;

    explicit ZeroLimitAudioProcessorEditor(ZeroLimitAudioProcessor&);
    ~ZeroLimitAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void setScaleFactor(float newScale) override;

private:
    void timerCallback() override;

    // リソースプロバイダ（Prod: 埋め込み ZIP, Dev: localhost:5173）
    using Resource = juce::WebBrowserComponent::Resource;
    std::optional<Resource> getResource(const juce::String& url) const;

    // ネイティブ関数ハンドラ（任意の UI → ネイティブ連携用）
    void handleSystemAction(const juce::Array<juce::var>& args,
                            juce::WebBrowserComponent::NativeFunctionCompletion completion);

    ZeroLimitAudioProcessor& audioProcessor;

    // Web パラメータリレー（WebBrowserComponent より先に宣言）
    juce::WebSliderRelay       webThresholdRelay;
    juce::WebSliderRelay       webOutputGainRelay;
    juce::WebSliderRelay       webReleaseMsRelay;
    juce::WebToggleButtonRelay webAutoReleaseRelay;
    juce::WebToggleButtonRelay webLinkRelay;
    juce::WebComboBoxRelay     webMeteringModeRelay;
    juce::WebComboBoxRelay     webModeRelay;       // Single / Multi band mode
    juce::WebComboBoxRelay     webBandCountRelay;  // 3 / 4 / 5 band count (Multi only)
    juce::WebComboBoxRelay     webDisplayModeRelay; // Metering / Waveform

    // APVTS ←→ Web バインディング
    juce::WebSliderParameterAttachment       thresholdAttachment;
    juce::WebSliderParameterAttachment       outputGainAttachment;
    juce::WebSliderParameterAttachment       releaseMsAttachment;
    juce::WebToggleButtonParameterAttachment autoReleaseAttachment;
    juce::WebToggleButtonParameterAttachment linkAttachment;
    juce::WebComboBoxParameterAttachment     meteringModeAttachment;
    juce::WebComboBoxParameterAttachment     modeAttachment;
    juce::WebComboBoxParameterAttachment     bandCountAttachment;
    juce::WebComboBoxParameterAttachment     displayModeAttachment;

    juce::WebControlParameterIndexReceiver controlParameterIndexReceiver;

    struct WebViewLifetimeGuard : public juce::WebViewLifetimeListener
    {
        std::atomic<bool> constructed{ false };
        void webViewConstructed(juce::WebBrowserComponent*) override { constructed.store(true, std::memory_order_release); }
        void webViewDestructed(juce::WebBrowserComponent*) override  { constructed.store(false, std::memory_order_release); }
        bool isConstructed() const { return constructed.load(std::memory_order_acquire); }
    } webViewLifetimeGuard;

    juce::WebBrowserComponent webView;

    bool useLocalDevServer = false;

    std::unique_ptr<juce::ResizableCornerComponent> resizer;
    juce::ComponentBoundsConstrainer resizerConstraints;

    // --- Linux 限定のウィンドウ制御（[[linux-dpi-resize-scaling]] と同方針）---
    //  Bitwig 等はホスト枠ドラッグをプラグインへ転送しないため、Linux では枠リサイズを無効化し
    //  （setResizable(false)）、自前ハンドル（window_action.resizeTo→setSize→host request_resize）
    //  のみ許可する。高頻度リサイズで取り残された黒残り/見切れは、ホストの echo 待ち（バック
    //  プレッシャ）と落ち着き後の 1px ジグル再同期で収束させる。Windows/macOS は従来どおり。
    void applyWindowResize(int targetW, int targetH,
                           juce::WebBrowserComponent::NativeFunctionCompletion completion);
    void applyDisplayScale();
    void resolveResizeAck();
    bool   resizeAckPending { false };
    bool   resizeSelfDriven { false };
    juce::uint32 resizeAckStartMs { 0 };
    juce::WebBrowserComponent::NativeFunctionCompletion pendingResizeCompletion;
    juce::uint32 lastResizeActivityMs { 0 };
    bool   settleReconcileDone { true };
    bool   resyncStep2Pending { false };
    int    resyncTargetW { 0 };
    int    resyncTargetH { 0 };
    // CSS px → 論理 px の換算比率（resizeBegin/apply_layout で確定し resizeTo/初期サイズに適用）。
    //  分数スケーリング環境でハンドル(CSS px)とウィンドウ(論理px)、初期サイズのズレを防ぐ（MixCompare 方式）。
    double webResizeRatioW { 1.0 };
    double webResizeRatioH { 1.0 };
    double lastWebViewDpr { -1.0 }; // WebUI が apply_layout で報告する devicePixelRatio（真のディスプレイ倍率）
    // APVTS state の保存サイズ（editorWidth/editorHeight）から復元したか。
    //  復元した場合、apply_layout の初回 ×ratio リサイズで保存値（論理px）を上書きしない（二重適用防止）。
    bool   restoredFromSavedSize { false };
    int    designTargetW { 453 };
    int    designTargetH { 470 };

    std::atomic<bool> isShuttingDown{ false };

#if defined(JUCE_WINDOWS)
    // DPI 変化検出: ディスプレイ間移動で HWND 基準の DPI が変わったときに、
    //   強制的に再レイアウトを走らせて画面の見切れを防ぐ。
    double lastHwndScaleFactor { 0.0 };
    int    lastHwndDpi         { 0 };
    void   pollAndMaybeNotifyDpiChange();
#endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ZeroLimitAudioProcessorEditor)
};
