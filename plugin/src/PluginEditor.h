#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginProcessor.h"
#include <optional>

class ZeroLimitAudioProcessorEditor : public juce::AudioProcessorEditor,
                                      private juce::Timer
{
public:
    explicit ZeroLimitAudioProcessorEditor(ZeroLimitAudioProcessor&);
    ~ZeroLimitAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

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
    juce::WebSliderRelay webThresholdRelay;
    juce::WebSliderRelay webOutputGainRelay;

    // APVTS ←→ Web バインディング
    juce::WebSliderParameterAttachment thresholdAttachment;
    juce::WebSliderParameterAttachment outputGainAttachment;

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

    std::atomic<bool> isShuttingDown{ false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ZeroLimitAudioProcessorEditor)
};
