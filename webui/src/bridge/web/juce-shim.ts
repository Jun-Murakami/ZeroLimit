/**
 * juce-framework-frontend-mirror の Web 互換 shim。
 * Vite エイリアスで本家モジュールの代わりにこれが解決される。
 *
 * 既存コンポーネントの import をそのまま動かすために、
 * 同じ関数名・同じ戻り値の形を維持する。
 *
 * ZeroLimit の APVTS パラメータ（THRESHOLD / OUTPUT_GAIN / RELEASE_MS /
 * AUTO_RELEASE / LINK / METERING_MODE / MODE / BAND_COUNT）を Web 側で
 * エミュレートし、値変化を WebAudioEngine へ直送する。
 */

import {
  WebSliderState,
  WebToggleState,
  WebComboBoxState,
} from './WebParamState';
import { webAudioEngine } from './WebAudioEngine';

// ---------- パラメータレジストリ ----------

const sliderStates   = new Map<string, WebSliderState>();
const toggleStates   = new Map<string, WebToggleState>();
const comboBoxStates = new Map<string, WebComboBoxState>();

function registerDefaults(): void
{
  // --- THRESHOLD / OUTPUT_GAIN: -30..0 dB 線形（プラグインと同一） ---
  const makeDbSlider = (defaultDb: number) => new WebSliderState({
    defaultScaled: defaultDb,
    min: -30,
    max: 0,
    toScaled:   (n: number) => -30 + n * 30,
    fromScaled: (db: number) => (db - -30) / 30,
  });
  sliderStates.set('THRESHOLD',   makeDbSlider(0));
  sliderStates.set('OUTPUT_GAIN', makeDbSlider(0));

  // --- RELEASE_MS: 0.01..1000 ms 線形（ReleaseSection 側で log→ms→linear 変換済み） ---
  sliderStates.set('RELEASE_MS', new WebSliderState({
    defaultScaled: 1.0,
    min: 0.01,
    max: 1000,
    toScaled:   (n: number) => 0.01 + n * (1000 - 0.01),
    fromScaled: (ms: number) => (ms - 0.01) / (1000 - 0.01),
  }));

  // --- トグル ---
  toggleStates.set('AUTO_RELEASE', new WebToggleState(true));
  toggleStates.set('LINK',         new WebToggleState(false));

  // --- コンボボックス ---
  comboBoxStates.set('METERING_MODE', new WebComboBoxState(0, 3)); // Peak / RMS / Momentary
  comboBoxStates.set('MODE',          new WebComboBoxState(1, 2)); // Single / Multi（既定 Multi）
  comboBoxStates.set('BAND_COUNT',    new WebComboBoxState(0, 3)); // 3 / 4 / 5（既定 3）

  // --- パラメータ変更 → WebAudioEngine 連携 ---

  sliderStates.get('THRESHOLD')!.valueChangedEvent.addListener(() => {
    webAudioEngine.setThresholdDb(sliderStates.get('THRESHOLD')!.getScaledValue());
  });
  sliderStates.get('OUTPUT_GAIN')!.valueChangedEvent.addListener(() => {
    webAudioEngine.setOutputGainDb(sliderStates.get('OUTPUT_GAIN')!.getScaledValue());
  });
  sliderStates.get('RELEASE_MS')!.valueChangedEvent.addListener(() => {
    webAudioEngine.setReleaseMs(sliderStates.get('RELEASE_MS')!.getScaledValue());
  });

  toggleStates.get('AUTO_RELEASE')!.valueChangedEvent.addListener(() => {
    webAudioEngine.setAutoRelease(toggleStates.get('AUTO_RELEASE')!.getValue());
  });
  // LINK は UI ローカル挙動（App.tsx 側で Threshold ⇔ Output を連動させる）なので DSP 送信不要

  comboBoxStates.get('METERING_MODE')!.valueChangedEvent.addListener(() => {
    webAudioEngine.setMeteringMode(comboBoxStates.get('METERING_MODE')!.getChoiceIndex());
  });
  comboBoxStates.get('MODE')!.valueChangedEvent.addListener(() => {
    webAudioEngine.setMode(comboBoxStates.get('MODE')!.getChoiceIndex() === 1);
  });
  comboBoxStates.get('BAND_COUNT')!.valueChangedEvent.addListener(() => {
    const idx = comboBoxStates.get('BAND_COUNT')!.getChoiceIndex();
    const n = idx === 0 ? 3 : idx === 1 ? 4 : 5;
    webAudioEngine.setBandCount(n);
  });

  // 初期値を WASM に反映（WASM 側は prepare 直後に既定 0/0/1/true/Multi/3）
  // WASM 初期化完了後に再送する余地はあるが、engine が未初期化なら postMessage は noop になるので害はない。
  webAudioEngine.setThresholdDb(sliderStates.get('THRESHOLD')!.getScaledValue());
  webAudioEngine.setOutputGainDb(sliderStates.get('OUTPUT_GAIN')!.getScaledValue());
  webAudioEngine.setReleaseMs(sliderStates.get('RELEASE_MS')!.getScaledValue());
  webAudioEngine.setAutoRelease(toggleStates.get('AUTO_RELEASE')!.getValue());
  webAudioEngine.setMode(comboBoxStates.get('MODE')!.getChoiceIndex() === 1);
  webAudioEngine.setBandCount(3);
  webAudioEngine.setMeteringMode(0);
}

registerDefaults();

// ---------- juce-framework-frontend-mirror 互換 API ----------

export function getSliderState(id: string): WebSliderState | null
{
  return sliderStates.get(id) ?? null;
}

export function getToggleState(id: string): WebToggleState | null
{
  return toggleStates.get(id) ?? null;
}

export function getComboBoxState(id: string): WebComboBoxState | null
{
  return comboBoxStates.get(id) ?? null;
}

export function getNativeFunction(
  _name: string,
): ((...args: unknown[]) => Promise<unknown>) | null
{
  // Web 版のネイティブ関数は WebBridgeManager 経由で処理する
  return null;
}
