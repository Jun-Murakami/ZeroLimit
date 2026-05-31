// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
//
// dB / LKFS の数値表示フォーマット。
//  コンポーネントファイル（VUMeter.tsx）から分離して react-refresh の
//  only-export-components 制約を満たす（HMR 境界を壊さない）。

// 表示上の下限。これ未満は -∞ 表記にする。
export const MIN_DB = -30;
export const LOUDNESS_MIN_LKFS = -60;

export const formatDb = (db: number): string =>
  db <= MIN_DB ? '-∞' : Math.max(MIN_DB, Math.min(0, db)).toFixed(1);

// Momentary LKFS 値用の数値表示（-∞ から 0 LKFS）
export const formatLkfs = (lkfs: number): string =>
  lkfs <= LOUDNESS_MIN_LKFS ? '-∞' : Math.max(LOUDNESS_MIN_LKFS, Math.min(0, lkfs)).toFixed(1);
