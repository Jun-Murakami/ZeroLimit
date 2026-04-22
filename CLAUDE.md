必ず日本語で回答すること。

## ZeroLimit 開発用 ルール（AGENTS）

この文書は JUCE + WebView（Vite/React/MUI）構成で「ゼロレイテンシー・ブロードキャスト用リミッター」を実装するための合意ルールです。開発時の意思決定や PR レビューの基準として用います。

### 目的とスコープ

- **目的**: ブロードキャスト最終段向けに、ルックアヘッド無しでピークを即時抑制するシンプルなブリックウォール・リミッター
- **対象フォーマット**: VST3 / AU / AAX / Standalone
- **最小要件**:
  - Threshold（-40..0 dBFS）
  - Output Gain（-24..0 dB、下方向トリムのみ）
  - Input L/R、GR、Output L/R の 5 本メーター（トゥルーピーク相当の区間最大）

### アーキテクチャ

- **C++/JUCE**: `PluginProcessor` が APVTS を保持、`ZeroLatencyLimiter` が DSP。メーター値は `std::atomic<float>` で audio→UI に受け渡し
- **WebUI**: Threshold / Output Gain は `WebSliderRelay` + `WebSliderParameterAttachment` で APVTS と双方向同期。メーターは `emitEventIfBrowserIsVisible("meterUpdate", ...)` で 30Hz 送出
- **リミッタ方針**: アタック即時（`targetGain = threshold / |x|`）、リリースは時定数ベース。L/R 共通のゲイン（リンク）で位相崩れを防止

### オーディオスレッド原則

- `processBlock` 内でのメモリ確保・ロック・ファイル I/O は禁止
- メーター蓄積は `compare_exchange_weak` で区間最大を保持し、UI タイマーで `exchange(0)` して取り出し
- パラメータの読み取りは `getRawParameterValue(...)->load()` を使用し、`AudioProcessorValueTreeState::Listener` は使わない（UI スレッドからのコールバック発生を避ける）

### UI/UX 原則

- ダークテーマ前提。MUI v7、`@fontsource/jost` をデフォルトフォントに使用
- メーターは HiDPI 対応 canvas で描画。視覚的な非線形スケール（中央 -24dB）
- フェーダーは `ParameterFader` に一本化（Threshold/OutputGain 両方で共通）
- 数値入力欄は `block-host-shortcuts` クラスでキーイベントの DAW 転送を抑制

### ブリッジ / メッセージ設計

- JS → C++（コマンド系）:
  - `system_action("ready")` — 初期化完了通知
  - `open_url(url)` — 外部 URL の起動
- C++ → JS（イベント系、30Hz スロットル）:
  - `meterUpdate`: `{ input: { truePeakLeft, truePeakRight }, output: {...}, grDb }`

### コーディング規約（C++）

- 明示的な型、早期 return、2 段以上の深いネスト回避
- 例外は原則不使用。戻り値でエラー伝搬
- コメントは「なぜ」を中心に要点のみ

### コーディング規約（Web）

- TypeScript 必須。any 型は禁止
- ESLint + Prettier。コンポーネントは疎結合・小さく
- MUI テーマはダーク優先

### ビルド

- Dev: WebView は `http://127.0.0.1:5173`
- Prod: `webui build` を zip 化 → `juce_add_binary_data` で埋め込み
- AAX SDK は `aax-sdk/` 配下に配置された場合のみ自動的に有効化
