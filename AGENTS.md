# ZeroLimit — AGENTS

このリポジトリで作業する AI コーディングエージェント向けの要点サマリです。詳細は [CLAUDE.md](./CLAUDE.md) を参照してください。

## プロジェクト

- JUCE + WebView（Vite/React/MUI）のオーディオプラグイン
- ゼロレイテンシー（ルックアヘッド無し）のブロードキャスト用ブリックウォール・リミッター
- パラメータは Threshold / Output Gain の 2 つだけ

## 作業するうえでの原則

1. オーディオスレッド上では確保/ロック/IO を行わない
2. パラメータは APVTS に集約、UI との双方向同期は `WebSliderRelay` + `WebSliderParameterAttachment` で行う
3. 高頻度データ（メーター）は `std::atomic<float>` で audio→UI へ渡し、UI タイマーで 30Hz に落とす
4. TypeScript は `any` 禁止
5. 新規ファイル/コンポーネントは `plugin/src/` または `webui/src/` のルールに従う

## ビルド

- Windows: `cmake --preset vs2022 && cmake --build --preset vs2022-release`
- macOS: `cmake --preset xcode && cmake --build --preset xcode-release`
- WebUI: `cd webui && npm install && npm run dev`（開発時）／`npm run build`（本番用埋め込みに必要）
