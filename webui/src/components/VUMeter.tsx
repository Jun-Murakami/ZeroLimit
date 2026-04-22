import React, { useEffect, useRef } from 'react';
import { Box, Typography } from '@mui/material';

// メーター描画定数
const METER_HEIGHT = 160;
const DEFAULT_BAR_WIDTH = 24;
const MIN_DB = -60;
const TARGET_DB_AT_MID = -24;
const LINEAR_AT_TARGET = (TARGET_DB_AT_MID - MIN_DB) / (0 - MIN_DB);
const VISUAL_EXPONENT = Math.log(0.5) / Math.log(Math.max(1e-6, LINEAR_AT_TARGET));

const dbToUnit = (db: number): number => {
  const clamped = Math.max(MIN_DB, Math.min(0, db));
  const linear = (clamped - MIN_DB) / (0 - MIN_DB);
  return Math.pow(linear, VISUAL_EXPONENT);
};
const dbToY = (db: number, h: number): number => h - dbToUnit(db) * h;

const setupHiDPICanvas = (canvas: HTMLCanvasElement, cssW: number, cssH: number): CanvasRenderingContext2D | null => {
  const dpr = Math.max(1, window.devicePixelRatio || 1);
  canvas.style.width = `${cssW}px`;
  canvas.style.height = `${cssH}px`;
  canvas.width = Math.round(cssW * dpr);
  canvas.height = Math.round(cssH * dpr);
  const ctx = canvas.getContext('2d');
  if (!ctx) return null;
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.imageSmoothingEnabled = false;
  return ctx;
};

const crisp = (v: number, dpr: number): number => Math.round(v * dpr) / dpr + 0.5 / dpr;

// ============================
// レベルメーター（dB, 上=0dBFS）
// ============================
export const LevelMeterBar: React.FC<{ level: number; label: string; width?: number }> = ({ level, label, width }) => {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const cssW = width ?? DEFAULT_BAR_WIDTH;

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const cssH = METER_HEIGHT;
    const ctx = setupHiDPICanvas(canvas, cssW, cssH);
    if (!ctx) return;
    const dpr = Math.max(1, window.devicePixelRatio || 1);

    ctx.clearRect(0, 0, cssW, cssH);
    ctx.fillStyle = '#333';
    ctx.fillRect(0, 0, cssW, cssH);

    const grad = ctx.createLinearGradient(0, cssH, 0, 0);
    grad.addColorStop(0, '#4fc3f7');
    grad.addColorStop(0.6, '#4fc3f7');
    grad.addColorStop(0.8, '#ffeb3b');
    grad.addColorStop(1, '#ff5252');

    const clamped = Math.max(MIN_DB, Math.min(0, level));
    const yTop = dbToY(clamped, cssH);
    ctx.fillStyle = grad;
    ctx.fillRect(0, yTop, cssW, cssH - yTop);

    ctx.strokeStyle = '#666';
    ctx.lineWidth = 0.5;
    [0, -6, -12, -18, -24, -30, -40, -50, -60].forEach((db) => {
      const y = dbToY(db, cssH);
      ctx.beginPath();
      const ya = crisp(y, dpr);
      ctx.moveTo(0, ya);
      ctx.lineTo(cssW * 0.25, ya);
      ctx.stroke();
    });
  }, [level, cssW]);

  return (
    <Box sx={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 0.5 }}>
      <canvas
        ref={canvasRef}
        width={cssW}
        height={METER_HEIGHT}
        style={{ borderRadius: 2, border: '1px solid #333', width: cssW, height: METER_HEIGHT }}
      />
      <Typography variant='caption' sx={{ fontSize: '9px', color: 'text.secondary', fontWeight: 500 }}>
        {label}
      </Typography>
    </Box>
  );
};

// ============================
// GR メーター（上=0dB、下方向にリダクション量）
// ============================
const GR_MAX_DB = 24; // 表示上限
const grToUnit = (grDb: number): number => Math.max(0, Math.min(1, grDb / GR_MAX_DB));

export const GainReductionMeterBar: React.FC<{ grDb: number; width?: number }> = ({ grDb, width }) => {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const cssW = width ?? DEFAULT_BAR_WIDTH * 2;

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const cssH = METER_HEIGHT;
    const ctx = setupHiDPICanvas(canvas, cssW, cssH);
    if (!ctx) return;
    const dpr = Math.max(1, window.devicePixelRatio || 1);

    ctx.clearRect(0, 0, cssW, cssH);
    ctx.fillStyle = '#333';
    ctx.fillRect(0, 0, cssW, cssH);

    const u = grToUnit(grDb);
    const barHeight = u * cssH;
    const grad = ctx.createLinearGradient(0, 0, 0, cssH);
    grad.addColorStop(0, '#ff5252');
    grad.addColorStop(0.4, '#ffab00');
    grad.addColorStop(1, '#ffab00');

    ctx.fillStyle = grad;
    ctx.fillRect(0, 0, cssW, barHeight);

    ctx.strokeStyle = '#666';
    ctx.lineWidth = 0.5;
    [0, 3, 6, 9, 12, 18, 24].forEach((db) => {
      const y = grToUnit(db) * cssH;
      ctx.beginPath();
      const ya = crisp(y, dpr);
      ctx.moveTo(0, ya);
      ctx.lineTo(cssW * 0.25, ya);
      ctx.stroke();
    });
  }, [grDb, cssW]);

  return (
    <Box sx={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 0.5 }}>
      <canvas
        ref={canvasRef}
        width={cssW}
        height={METER_HEIGHT}
        style={{ borderRadius: 2, border: '1px solid #333', width: cssW, height: METER_HEIGHT }}
      />
      <Typography variant='caption' sx={{ fontSize: '9px', color: 'text.secondary', fontWeight: 500 }}>
        GR
      </Typography>
    </Box>
  );
};

// ============================
// ラベル付き dB 表示
// ============================
export const formatDb = (db: number): string => (db <= MIN_DB ? '-∞' : Math.max(MIN_DB, Math.min(0, db)).toFixed(1));
