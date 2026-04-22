必ず日本語で回答すること。

## ZeroLimit 開発用 ルール（AGENTS）

この文書は JUCE + WebView（Vite/React/MUI）構成で「ゼロレイテンシー・ブロードキャスト／マスタリング用リミッター」を実装するための合意ルールです。開発時の意思決定や PR レビューの基準として用います。

### 目的とスコープ

- **目的**: ルックアヘッド無しでピークを即時抑制するブリックウォール・リミッター。放送／配信／ライブ用途のゼロレイテンシーと、音楽マスタリング向けのマルチバンド処理の両立。
- **対象フォーマット**: VST3 / AU / AAX / Standalone
- **機能要件**:
  - Threshold / Output Gain フェーダー（-30..0 dB、L2 準拠レンジ）
  - Link（Threshold ⇔ Output Gain の相対オフセット連動）
  - Auto / Manual Release（0.01..1000 ms、log skew、dual envelope の min() で program-dependent）
  - Single / Multi バンドモード切替（Multi は 3 / 4 / 5 バンドから選択）
  - Input / GR / Output メーター（Peak / RMS / Momentary LKFS 切替）
  - Auto Makeup Gain（Threshold を下げた分を自動補償）

### アーキテクチャ

- **C++/JUCE**:
  - `PluginProcessor` が APVTS を保持、`processBlock` で DSP チェーンを実行
  - `ZeroLatencyLimiter`（Single）/ `MultibandLimiter`（Multi, 3/4/5 band 可変）の切替
  - マルチバンド時は `multibandLimiter` → `limiter`（サム後の最終セーフティ）の 2 段構成
  - `CrossoverLR4` が LR4 IIR ツリー分割 + 位相アライメント用 allpass 連鎖を担当
  - メーター値は `std::atomic<float>` で audio → UI に受け渡し（区間最大を `compare_exchange_weak` で更新）
- **WebUI**: 
  - APVTS とは `useJuceParam.ts` 経由で `useSyncExternalStore` 購読（tearing-free）
  - フェーダー / トグル / コンボは Web*Relay + Web*ParameterAttachment で双方向同期
  - メーターは 30Hz で `meterUpdate` イベントを購読して canvas 描画
- **リミッタ方針**: アタック即時（`targetGain = threshold / |x|`）、リリースは時定数ベース。L/R 共通のゲイン（リンク）で位相崩れを防止

### マルチバンド構成

- クロスオーバーは **Linkwitz-Riley 4th order IIR**（BW2 を 2 段カスケード、Q = 1/√2）
- tree cascade: `Input → LR4@c_0 → {B_0, R_1}; R_i → LR4@c_i → {B_i, R_{i+1}}; ...`
- 位相アライメント: バンド B_b には c_{b+1}, c_{b+2}, ... の allpass を連続適用（`AP_LR4 = LP_LR4 + HP_LR4`）
- バンド数ごとの既定クロスオーバーと時定数は `MultibandLimiter.cpp` の匿名名前空間で固定（ゼロコンフィグ）
  - **3-band**: 120 Hz / 5 kHz。声を Mid に閉じ込める。放送向け
  - **4-band**: 150 Hz / 5 kHz / 15 kHz（Steinberg 準拠）。声保持 + Air 分離
  - **5-band**: 80 / 250 / 1k / 5k Hz（UA 準拠）。音楽マスタリング志向
- Multi モード時は `AUTO_RELEASE` を強制 ON として扱い、手動 `RELEASE_MS` は無視

### オーディオスレッド原則

- `processBlock` 内でのメモリ確保・ロック・ファイル I/O は禁止
- メーター蓄積は `compare_exchange_weak` で区間最大を保持し、UI タイマーで `exchange(0)` して取り出し
- パラメータの読み取りは `getRawParameterValue(...)->load()` を使用し、`AudioProcessorValueTreeState::Listener` は使わない（UI スレッドからのコールバック発生を避ける）
- マルチバンド用の作業バッファは `prepare()` で最大ブロックサイズ分を事前確保し、`processBlock` 中は `setSize(..., avoidReallocating=true)` で再割り当てを発生させない

### UI/UX 原則

- ダークテーマ前提。MUI v7、`@fontsource/jost` をデフォルトフォントに使用
- メーターは HiDPI 対応 canvas で描画。視覚的な非線形スケール（中央 -15..-24 dB）
- フェーダーは `ParameterFader` に一本化（Threshold / OutputGain 両方で共通）
- 数値入力欄は `block-host-shortcuts` クラスでキーイベントの DAW 転送を抑制
- Multi モードでは Release セクションを半透明化 + `pointer-events: none` で無効化表示
- 初心者向け配慮: バンド数選択は「3 / 4 / 5」の数字だけでは意味が伝わらないので `Bands` ラベルを添える。クロスオーバー周波数などの内部パラメータは UI に露出しない（ゼロコンフィグ）
- 既定値: Multi-band ON / 3 Bands / Threshold 0 / Output 0 / Link OFF / Auto Release ON

### ブリッジ / メッセージ設計

- JS → C++（コマンド系、`callNative` 経由）:
  - `system_action("ready")` — 初期化完了通知
  - `system_action("forward_key_event", payload)` — キー転送
  - `open_url(url)` — 外部 URL の起動
  - `window_action("resizeTo", w, h)` — Standalone 用リサイズ
- C++ → JS（イベント系、30Hz スロットル）:
  - `meterUpdate`: `{ input: {...}, output: {...}, grDb, meteringMode }`
    - mode 0 (Peak): `truePeakLeft / truePeakRight`
    - mode 1 (RMS): `rmsLeft / rmsRight`
    - mode 2 (Momentary): `momentary`（LKFS）

### パラメータ一覧（APVTS）

- `THRESHOLD`: float, -30..0 dB, 既定 0
- `OUTPUT_GAIN`: float, -30..0 dB, 既定 0
- `RELEASE_MS`: float, 0.01..1000 ms, log skew（lambda 形式 NormalisableRange）, 既定 1.0
  - ⚠ frontend-mirror は lambda 形式 skew を認識できず skew=1 の線形換算になる。UI 側で「log 0..1 → ms → 線形 0..1」に変換して `setNormalisedValue` に渡す（`ReleaseSection.tsx:applyNormalised` 参照）
- `AUTO_RELEASE`: bool, 既定 true
- `LINK`: bool, 既定 false
- `METERING_MODE`: choice [Peak, RMS, Momentary], 既定 Peak
- `MODE`: choice [Single, Multi], 既定 **Multi**
- `BAND_COUNT`: choice [3 Band, 4 Band, 5 Band], 既定 **3 Band**

### React 設計方針

- 外部ストア購読は `useSyncExternalStore`（`hooks/useJuceParam.ts`）。tearing-free で StrictMode 安全。
- `App.tsx` は THRESHOLD / OUTPUT_GAIN の state 参照のみ必要（mirror ロジック用）なので、value 購読しない `useJuceSliderState` を使い、App 全体の不要な再レンダーを避ける。
- `useEffect` は最小限。STEP1: React 18/19 フックで代替できないか検討 / STEP2: アンチパターン該当チェック / STEP3: 残った正当な useEffect はクリーンアップ必須。
- `useEffectEvent` は stable だが、JUCE の `valueChangedEvent` から呼ぶと commit タイミングの race でワブリング／ループ発生の実績あり。**JUCE 由来のコールバックでは Latest Ref Pattern を使う**のが安全。
- Latest Ref Pattern: `const xRef = useRef(x); xRef.current = x;` を render 中に実行。

### コーディング規約（C++）

- 明示的な型、早期 return、2 段以上の深いネスト回避
- 例外は原則不使用。戻り値でエラー伝搬
- コメントは「なぜ」を中心に要点のみ
- 新規 DSP クラスは `plugin/src/dsp/` 配下、`namespace zl::dsp` で統一

### コーディング規約（Web）

- TypeScript 必須。any 型は禁止
- ESLint + Prettier。コンポーネントは疎結合・小さく
- MUI テーマはダーク優先
- `useEffect` の新規追加時は必ず `useeffect-guard` の STEP1/2/3 チェックを通す

### ビルド

- Dev: WebView は `http://127.0.0.1:5173`（Vite dev server）
- Prod: `webui build` を zip 化 → `juce_add_binary_data` で埋め込み
- AAX SDK は `aax-sdk/` 配下に配置された場合のみ自動的に有効化
- Windows 配布ビルド: `powershell -File build_windows.ps1 -Configuration Release`
  - 成果物: `releases/<VERSION>/ZeroLimit_<VERSION>_Windows_VST3_AAX_Standalone.zip` と `ZeroLimit_<VERSION>_Windows_Setup.exe`（Inno Setup 6 必須）
  - AAX 署名は `.env` に PACE 情報がある場合のみ自動実行

### バージョン管理

- `VERSION` ファイルで一元管理。CMake と `build_windows.ps1` がここから読む
- `webui/package.json` の `version` も手動で同期する
- コミットは**ユーザが明示的に指示しない限り行わない**（`memory/feedback_git_commits.md` に記録済）

### デフォルト挙動メモ

- 新規インスタンス時は Multi-band ON / 3 Bands / Auto Release ON で立ち上がる（ゼロコンフィグで即座に"良い音"が出る方向）
- Threshold 0 dB 既定 = バイパス相当（ユーザが明示的に下げるまで音に触れない）
- プラグインウィンドウ最小 410 × 390、初期 470 × 470
