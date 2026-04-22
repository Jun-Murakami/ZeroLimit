# ZeroLimit

ゼロレイテンシー・ブロードキャスト用リミッター。JUCE + WebView（Vite/React/MUI）構成の VST3 / AU / AAX / Standalone プラグイン。

## 特徴

- **0 サンプル レイテンシー**: ルックアヘッド無し。瞬間ピークに即応するブリックウォール方式
- **シンプルな UI**: Threshold / Output Gain のフェーダー 2 本と Input L/R・GR・Output L/R の 5 本メーター
- **フォーマット**: VST3 / AU / AAX / Standalone（AAX は SDK 配置時のみビルド）

## ディレクトリ構成

- `plugin/` — JUCE プラグイン（C++）
  - `src/PluginProcessor.*` — DSP エントリ（APVTS, メーター集計）
  - `src/PluginEditor.*` — WebView 初期化、APVTS↔WebUI のリレー、メーター送出タイマー
  - `src/dsp/Limiter.*` — ゼロレイテンシー・ブリックウォール・リミッター
- `webui/` — Vite + React 19 + MUI 7 のフロントエンド
- `cmake/` — Version.cmake とアイコン
- `JUCE/` — サブモジュール
- `aax-sdk/` — （ローカル専用。Avid からの個別入手が必要。存在すれば AAX ビルドが有効化）

## セットアップ

```bash
# 1. クローン & サブモジュール
git clone <this-repo>
cd ZeroLimit
git submodule update --init --recursive

# 2. WebUI 依存
cd webui && npm install && cd ..

# 3. CMake configure / build
cmake --preset vs2022        # Windows
cmake --build --preset vs2022-release
# あるいは
cmake --preset xcode         # macOS
cmake --build --preset xcode-release
```

## 開発モード（Vite dev server + WebView）

```bash
# 1) WebUI dev server
cd webui && npm run dev
# 2) 別ターミナルで Debug ビルド/起動
cmake --build build --config Debug
```

Debug ビルド時は `LOCAL_DEV_SERVER_ADDRESS` = `http://127.0.0.1:5173` を WebView が読み込みます。Release ビルドでは `webui` の成果物を ZIP 化して `juce_add_binary_data` で埋め込みます。

## パラメータ

| ID            | 範囲           | 既定値 | 用途                                      |
| ------------- | -------------- | ------ | ----------------------------------------- |
| `THRESHOLD`   | -30 .. 0 dB   | 0 dB   | リミット基準レベル（これ以上は出力しない）  |
| `OUTPUT_GAIN` | -30 .. 0 dB   | 0 dB   | リミッター段後のトリム（下方向のみ）        |

※ レンジは業界標準の Waves L2 に揃えています。

## ライセンス

本体コードは LICENSE を参照。依存 SDK は各ベンダーのライセンスに従います。
