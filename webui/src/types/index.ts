// ZeroLimit: ゼロレイテンシー・リミッター用の型定義

// メーターレベル（dBFS スケール）
export interface StereoMeter {
  truePeakLeft?: number;
  truePeakRight?: number;
}

// JUCE → WebUI のメーター更新イベント
// - input:  入力段のトゥルーピーク相当（区間最大 dB）
// - output: 出力段のトゥルーピーク相当（区間最大 dB）
// - grDb:   区間最大のゲインリダクション（正値 dB, 0 = リダクションなし）
export interface MeterUpdateData {
  input?: StereoMeter;
  output?: StereoMeter;
  grDb?: number;
}

// JUCE Backend 型定義
declare class Backend {
  addEventListener(eventId: string, fn: (args: unknown) => unknown): [string, number];
  removeEventListener(param: [string, number]): void;
  emitEvent(eventId: string, object: unknown): void;
  emitByBackend(eventId: string, object: unknown): void;
}

declare global {
  interface Window {
    __JUCE__?: {
      backend: Backend;
      initialisationData: Record<string, unknown>;
      postMessage: () => void;
    };
    getNativeFunction?: (name: string) => (...args: unknown[]) => Promise<unknown>;
    getSliderState?: (name: string) => unknown;
    getToggleState?: (name: string) => unknown;
    getComboBoxState?: (name: string) => unknown;
    getBackendResourceAddress?: (path: string) => string;
    __resizeRAF?: number;
  }
}

export {};
