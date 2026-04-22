#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "ParameterIDs.h"
#include "Version.h"

#include <unordered_map>
#include <cmath>

#if __has_include(<WebViewFiles.h>)
#include <WebViewFiles.h>
#endif

#ifndef LOCAL_DEV_SERVER_ADDRESS
#define LOCAL_DEV_SERVER_ADDRESS "http://127.0.0.1:5173"
#endif

namespace {

std::vector<std::byte> streamToVector(juce::InputStream& stream)
{
    const auto sizeInBytes = static_cast<size_t>(stream.getTotalLength());
    std::vector<std::byte> result(sizeInBytes);
    stream.setPosition(0);
    [[maybe_unused]] const auto bytesRead = stream.read(result.data(), result.size());
    jassert(bytesRead == static_cast<ssize_t>(sizeInBytes));
    return result;
}

#if !ZEROLIMIT_DEV_MODE && __has_include(<WebViewFiles.h>)
static const char* getMimeForExtension(const juce::String& extension)
{
    static const std::unordered_map<juce::String, const char*> mimeMap = {
        {{"htm"},   "text/html"},
        {{"html"},  "text/html"},
        {{"txt"},   "text/plain"},
        {{"jpg"},   "image/jpeg"},
        {{"jpeg"},  "image/jpeg"},
        {{"svg"},   "image/svg+xml"},
        {{"ico"},   "image/vnd.microsoft.icon"},
        {{"json"},  "application/json"},
        {{"png"},   "image/png"},
        {{"css"},   "text/css"},
        {{"map"},   "application/json"},
        {{"js"},    "text/javascript"},
        {{"woff2"}, "font/woff2"}};

    if (const auto it = mimeMap.find(extension.toLowerCase()); it != mimeMap.end())
        return it->second;

    jassertfalse;
    return "";
}

#ifndef ZIPPED_FILES_PREFIX
#error "You must provide the prefix of zipped web UI files' paths via ZIPPED_FILES_PREFIX compile definition"
#endif

std::vector<std::byte> getWebViewFileAsBytes(const juce::String& filepath)
{
    juce::MemoryInputStream zipStream{ webview_files::webview_files_zip,
                                       webview_files::webview_files_zipSize,
                                       false };
    juce::ZipFile zipFile{ zipStream };

    const auto fullPath = ZIPPED_FILES_PREFIX + filepath;
    if (auto* zipEntry = zipFile.getEntry(fullPath))
    {
        const std::unique_ptr<juce::InputStream> entryStream{ zipFile.createStreamForEntry(*zipEntry) };
        if (entryStream == nullptr) { jassertfalse; return {}; }
        return streamToVector(*entryStream);
    }
    return {};
}
#else
[[maybe_unused]] static std::vector<std::byte> getWebViewFileAsBytes(const juce::String& filepath)
{
    juce::ignoreUnused(filepath);
    return {};
}
#endif

} // namespace

//==============================================================================

ZeroLimitAudioProcessorEditor::ZeroLimitAudioProcessorEditor(ZeroLimitAudioProcessor& p)
    : AudioProcessorEditor(&p),
      audioProcessor(p),
      webThresholdRelay  { zl::id::THRESHOLD.getParamID() },
      webOutputGainRelay { zl::id::OUTPUT_GAIN.getParamID() },
      thresholdAttachment  { *p.getState().getParameter(zl::id::THRESHOLD.getParamID()),   webThresholdRelay,  nullptr },
      outputGainAttachment { *p.getState().getParameter(zl::id::OUTPUT_GAIN.getParamID()), webOutputGainRelay, nullptr },
      webView{
          juce::WebBrowserComponent::Options{}
              .withBackend(juce::WebBrowserComponent::Options::Backend::webview2)
              .withWinWebView2Options(
                  juce::WebBrowserComponent::Options::WinWebView2{}
                      .withBackgroundColour(juce::Colour(0xFF606F77))
                      .withUserDataFolder(juce::File::getSpecialLocation(
                          juce::File::SpecialLocationType::tempDirectory)))
              .withWebViewLifetimeListener(&webViewLifetimeGuard)
              .withNativeIntegrationEnabled()
              .withInitialisationData("vendor", "ZeroLimit")
              .withInitialisationData("pluginName", "ZeroLimit")
              .withInitialisationData("pluginVersion", ZEROLIMIT_VERSION_STRING)
              .withOptionsFrom(controlParameterIndexReceiver)
              .withOptionsFrom(webThresholdRelay)
              .withOptionsFrom(webOutputGainRelay)
              .withNativeFunction(
                  juce::Identifier{"system_action"},
                  [this](const juce::Array<juce::var>& args,
                         juce::WebBrowserComponent::NativeFunctionCompletion completion)
                  { handleSystemAction(args, std::move(completion)); })
              .withNativeFunction(
                  juce::Identifier{"open_url"},
                  [](const juce::Array<juce::var>& args,
                     juce::WebBrowserComponent::NativeFunctionCompletion completion)
                  {
                      if (args.size() > 0)
                      {
                          const auto url = args[0].toString();
                          juce::URL(url).launchInDefaultBrowser();
                      }
                      completion(juce::var{ true });
                  })
              .withResourceProvider([this](const juce::String& url) { return getResource(url); })
      }
{
   #if ZEROLIMIT_DEV_MODE
    useLocalDevServer = true;
   #else
    useLocalDevServer = false;
   #endif

    addAndMakeVisible(webView);

    // 初期サイズ
    setSize(500, 420);

    // リサイズ可能に（Standalone 時の利便性）
    setResizable(true, false);
    resizerConstraints.setSizeLimits(392, 320, 1200, 900);
    resizer.reset(new juce::ResizableCornerComponent(this, &resizerConstraints));
    addAndMakeVisible(resizer.get());

    if (useLocalDevServer)
        webView.goToURL(LOCAL_DEV_SERVER_ADDRESS);
    else
        webView.goToURL(juce::WebBrowserComponent::getResourceProviderRoot());

    startTimerHz(30);
}

ZeroLimitAudioProcessorEditor::~ZeroLimitAudioProcessorEditor()
{
    isShuttingDown.store(true, std::memory_order_release);
    stopTimer();
}

void ZeroLimitAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xFF606F77));
}

void ZeroLimitAudioProcessorEditor::resized()
{
    webView.setBounds(getLocalBounds());
    if (resizer)
        resizer->setBounds(getWidth() - 16, getHeight() - 16, 16, 16);
}

std::optional<ZeroLimitAudioProcessorEditor::Resource>
ZeroLimitAudioProcessorEditor::getResource(const juce::String& url) const
{
   #if ZEROLIMIT_DEV_MODE
    juce::ignoreUnused(url);
    return std::nullopt;
   #else
    #if __has_include(<WebViewFiles.h>)
    const auto cleaned = url.startsWith("/") ? url.substring(1) : url;
    const auto resourcePath = cleaned.isEmpty() ? juce::String("index.html") : cleaned;
    const auto bytes = getWebViewFileAsBytes(resourcePath);
    if (bytes.empty())
        return std::nullopt;

    const auto extension = resourcePath.fromLastOccurrenceOf(".", false, false);
    return Resource{ std::move(bytes), juce::String(getMimeForExtension(extension)) };
    #else
    juce::ignoreUnused(url);
    return std::nullopt;
    #endif
   #endif
}

void ZeroLimitAudioProcessorEditor::handleSystemAction(const juce::Array<juce::var>& args,
                                                      juce::WebBrowserComponent::NativeFunctionCompletion completion)
{
    if (args.size() > 0)
    {
        const auto action = args[0].toString();
        // 現状は "ready" を受けて状態を返すだけ。必要に応じて拡張。
        if (action == "ready")
        {
            juce::DynamicObject::Ptr init{ new juce::DynamicObject{} };
            init->setProperty("pluginName", "ZeroLimit");
            init->setProperty("version", ZEROLIMIT_VERSION_STRING);
            completion(juce::var{ init.get() });
            return;
        }
    }
    completion(juce::var{});
}

void ZeroLimitAudioProcessorEditor::timerCallback()
{
    if (isShuttingDown.load(std::memory_order_acquire)) return;
    if (! webViewLifetimeGuard.isConstructed()) return;

    // 区間メーター値を取り出してリセット
    const float inL  = audioProcessor.inPeakAccumL.exchange(0.0f, std::memory_order_acq_rel);
    const float inR  = audioProcessor.inPeakAccumR.exchange(0.0f, std::memory_order_acq_rel);
    const float outL = audioProcessor.outPeakAccumL.exchange(0.0f, std::memory_order_acq_rel);
    const float outR = audioProcessor.outPeakAccumR.exchange(0.0f, std::memory_order_acq_rel);
    const float minGainLin = audioProcessor.minGainAccum.exchange(1.0f, std::memory_order_acq_rel);

    const double inLdB  = juce::Decibels::gainToDecibels(inL,  -60.0f);
    const double inRdB  = juce::Decibels::gainToDecibels(inR,  -60.0f);
    const double outLdB = juce::Decibels::gainToDecibels(outL, -60.0f);
    const double outRdB = juce::Decibels::gainToDecibels(outR, -60.0f);

    // GR は正値 dB（リダクション量）。minGainLin=1.0 なら 0 dB。
    const double grDb = (minGainLin >= 1.0f)
                            ? 0.0
                            : -static_cast<double>(juce::Decibels::gainToDecibels(minGainLin, -60.0f));

    juce::DynamicObject::Ptr meter{ new juce::DynamicObject{} };

    juce::DynamicObject::Ptr input{ new juce::DynamicObject{} };
    input->setProperty("truePeakLeft",  inLdB);
    input->setProperty("truePeakRight", inRdB);
    meter->setProperty("input", input.get());

    juce::DynamicObject::Ptr output{ new juce::DynamicObject{} };
    output->setProperty("truePeakLeft",  outLdB);
    output->setProperty("truePeakRight", outRdB);
    meter->setProperty("output", output.get());

    meter->setProperty("grDb", grDb);

    webView.emitEventIfBrowserIsVisible("meterUpdate", meter.get());
}
