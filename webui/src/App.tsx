import { useEffect, useRef, useState } from 'react';
import { Box, Button, Paper, Tooltip, Typography } from '@mui/material';
import { CssBaseline, ThemeProvider } from '@mui/material';
import { getComboBoxState } from 'juce-framework-frontend-mirror';
import { juceBridge } from './bridge/juce';
import { darkTheme } from './theme';
import { ParameterFader } from './components/ParameterFader';
import {
  GainReductionMeterBar,
  LevelMeterBar,
  LoudnessMeterBar,
  formatDb,
  formatLkfs,
} from './components/VUMeter';
import { ReleaseSection } from './components/ReleaseSection';
import { useHostShortcutForwarding } from './hooks/useHostShortcutForwarding';
import { GlobalDialog } from './components/GlobalDialog';
import LicenseDialog from './components/LicenseDialog';
import type { MeterUpdateData } from './types';
import './App.css';

// フェーダー/メーターの下限に合わせる（VUMeter.tsx 側と揃える）
const MIN_DB = -30;
const MIN_LKFS = -60;

type MeterMode = 'peak' | 'rms' | 'momentary';
const MODES: MeterMode[] = ['peak', 'rms', 'momentary'];
const MODE_LABEL: Record<MeterMode, string> = {
  peak: 'Peak',
  rms: 'RMS',
  momentary: 'Momentary',
};

function App() {
  useHostShortcutForwarding();

  // メーター現在値（バー描画用）
  const [inL, setInL] = useState(MIN_DB);
  const [inR, setInR] = useState(MIN_DB);
  const [outL, setOutL] = useState(MIN_DB);
  const [outR, setOutR] = useState(MIN_DB);
  const [grDb, setGrDb] = useState(0);
  const [inLkfs, setInLkfs] = useState(MIN_LKFS);
  const [outLkfs, setOutLkfs] = useState(MIN_LKFS);

  // ピークホールド（数値表示用、クリックでリセット）
  const [inHold, setInHold] = useState({ left: MIN_DB, right: MIN_DB });
  const [outHold, setOutHold] = useState({ left: MIN_DB, right: MIN_DB });
  const [grHold, setGrHold] = useState(0);
  const [inLkfsHold, setInLkfsHold] = useState(MIN_LKFS);
  const [outLkfsHold, setOutLkfsHold] = useState(MIN_LKFS);

  const clampDb = (db: number) => Math.max(MIN_DB, Math.min(0, db));
  const clampLkfs = (v: number) => Math.max(MIN_LKFS, Math.min(0, v));

  const resetInHold = () => {
    setInHold({ left: MIN_DB, right: MIN_DB });
    setInLkfsHold(MIN_LKFS);
  };
  const resetOutHold = () => {
    setOutHold({ left: MIN_DB, right: MIN_DB });
    setOutLkfsHold(MIN_LKFS);
  };
  const resetGrHold = () => setGrHold(0);

  // METERING_MODE パラメータ（APVTS 側）と双方向バインド
  const meteringCombo = getComboBoxState('METERING_MODE');
  const [meterModeIndex, setMeterModeIndex] = useState<number>(() =>
    meteringCombo ? meteringCombo.getChoiceIndex() : 0,
  );
  const meterMode: MeterMode = MODES[meterModeIndex] ?? 'peak';

  useEffect(() => {
    if (!meteringCombo) return;
    const id = meteringCombo.valueChangedEvent.addListener(() => {
      setMeterModeIndex(meteringCombo.getChoiceIndex());
    });
    return () => meteringCombo.valueChangedEvent.removeListener(id);
  }, [meteringCombo]);

  const cycleMeterMode = () => {
    const next = (meterModeIndex + 1) % MODES.length;
    setMeterModeIndex(next);
    meteringCombo?.setChoiceIndex(next);
  };

  useEffect(() => {
    const id = juceBridge.addEventListener('meterUpdate', (d: unknown) => {
      const m = d as MeterUpdateData;
      if (typeof m.meteringMode === 'number') setMeterModeIndex(m.meteringMode);

      if (m.meteringMode === 2) {
        const iL = m.input?.momentary ?? MIN_LKFS;
        const oL = m.output?.momentary ?? MIN_LKFS;
        setInLkfs(iL);
        setOutLkfs(oL);
        setInLkfsHold((p) => Math.max(p, clampLkfs(iL)));
        setOutLkfsHold((p) => Math.max(p, clampLkfs(oL)));
      } else {
        const isRms = m.meteringMode === 1;
        const iL = (isRms ? m.input?.rmsLeft  : m.input?.truePeakLeft)  ?? MIN_DB;
        const iR = (isRms ? m.input?.rmsRight : m.input?.truePeakRight) ?? MIN_DB;
        const oL = (isRms ? m.output?.rmsLeft  : m.output?.truePeakLeft)  ?? MIN_DB;
        const oR = (isRms ? m.output?.rmsRight : m.output?.truePeakRight) ?? MIN_DB;
        setInL(iL); setInR(iR); setOutL(oL); setOutR(oR);
        setInHold((p)  => ({ left: Math.max(p.left, clampDb(iL)), right: Math.max(p.right, clampDb(iR)) }));
        setOutHold((p) => ({ left: Math.max(p.left, clampDb(oL)), right: Math.max(p.right, clampDb(oR)) }));
      }

      const gr = m.grDb ?? 0;
      setGrDb(gr);
      setGrHold((p) => Math.max(p, gr));
    });
    return () => juceBridge.removeEventListener(id);
  }, []);

  useEffect(() => {
    juceBridge.whenReady(() => {
      juceBridge.callNative('system_action', 'ready');
    });

    const onContextMenu = (e: MouseEvent) => {
      const t = e.target as HTMLElement | null;
      if (!t) return;
      if (t.closest('input, textarea, select, [contenteditable="true"], .allow-contextmenu')) return;
      if (import.meta.env.DEV) return;
      e.preventDefault();
    };
    window.addEventListener('contextmenu', onContextMenu, { capture: true });
    return () => {
      window.removeEventListener('contextmenu', onContextMenu, { capture: true });
    };
  }, []);

  const [licenseOpen, setLicenseOpen] = useState(false);
  const openLicenseDialog = () => setLicenseOpen(true);
  const closeLicenseDialog = () => setLicenseOpen(false);

  // メインコントロール領域（Paper 内、Release セクションを除いた部分）のサイズを観測。
  //  フェーダー / メーターの高さとバー幅を動的に算出する。
  const mainRef = useRef<HTMLDivElement | null>(null);
  const [mainSize, setMainSize] = useState<{ width: number; height: number }>({ width: 520, height: 260 });
  useEffect(() => {
    const el = mainRef.current;
    if (!el) return;
    const ro = new ResizeObserver((entries) => {
      for (const entry of entries) {
        const w = entry.contentRect.width;
        const h = entry.contentRect.height;
        setMainSize((prev) => (prev.width !== w || prev.height !== h ? { width: w, height: h } : prev));
      }
    });
    ro.observe(el);
    return () => ro.disconnect();
  }, []);

  // --- 派生サイズ ---
  //   フェーダー / メーターの縦長部分 (slider rail or canvas) に割り当てる高さ。
  //   メーター列のヘッダ(36) + hold 行(14 + mt 2) + モード切替ボタン(24 + mt 8) の 84px を差し引く。
  const meterAndFaderHeight = Math.max(80, Math.floor(mainSize.height - 84));

  //   中央メーター領域の幅 = 全体幅 - フェーダー幅×2 (76×2) - grid の gap 2(=16)×2 = 全体 -184
  //   そこから GR バー幅(48) と IN/OUT 間のセンターギャップ(0.25×2 = 4) を差し引いて 2 等分。
  const meterAreaWidth = Math.max(0, mainSize.width - 76 * 2 - 16 * 2);
  const meterColumnWidth = Math.max(52, Math.floor((meterAreaWidth - 48 - 4) / 2));
  //   L/R ペアの各バー幅（gap 0.25 = 2px を引いて半分）
  const levelBarWidth = Math.max(24, Math.floor((meterColumnWidth - 2) / 2));

  // リサイズハンドル（Standalone 用）
  const dragState = useRef<{ startX: number; startY: number; startW: number; startH: number } | null>(null);
  const onDragStart: React.PointerEventHandler<HTMLDivElement> = (e) => {
    dragState.current = { startX: e.clientX, startY: e.clientY, startW: window.innerWidth, startH: window.innerHeight };
    e.currentTarget.setPointerCapture(e.pointerId);
  };
  const onDrag: React.PointerEventHandler<HTMLDivElement> = (e) => {
    if (!dragState.current) return;
    const dx = e.clientX - dragState.current.startX;
    const dy = e.clientY - dragState.current.startY;
    const w = Math.max(392, dragState.current.startW + dx);
    const h = Math.max(320, dragState.current.startH + dy);
    if (!window.__resizeRAF) {
      window.__resizeRAF = requestAnimationFrame(() => {
        window.__resizeRAF = 0;
        juceBridge.callNative('window_action', 'resizeTo', w, h);
      });
    }
  };
  const onDragEnd: React.PointerEventHandler<HTMLDivElement> = () => {
    dragState.current = null;
  };

  return (
    <ThemeProvider theme={darkTheme}>
      <CssBaseline />
      <style>{`
        html, body, #root {
          -webkit-user-select: none;
          -ms-user-select: none;
          user-select: none;
        }
        input, textarea, select, [contenteditable="true"], .allow-selection {
          -webkit-user-select: text !important;
          -ms-user-select: text !important;
          user-select: text !important;
          caret-color: auto;
        }
      `}</style>
      <Box sx={{ height: '100vh', display: 'flex', flexDirection: 'column', p: 2, pt: 0, overflow: 'hidden' }}>
        <Box sx={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', px: 1, py: 0.5 }}>
          <Typography
            variant='body2'
            component='div'
            sx={{ flexGrow: 1, color: 'primary.main', fontWeight: 600, cursor: 'pointer' }}
            onClick={openLicenseDialog}
            title='Licenses'
          >
            ZeroLimit
          </Typography>
          <Typography
            variant='caption'
            color='text.secondary'
            onClick={openLicenseDialog}
            sx={{ cursor: 'pointer' }}
            title='Licenses'
          >
            by Jun Murakami
          </Typography>
        </Box>

        <Paper
          elevation={2}
          sx={{
            p: 2,
            mb: 1,
            flex: 1,
            minHeight: 0,
            display: 'flex',
            flexDirection: 'column',
            alignItems: 'center',
            justifyContent: 'flex-start',
            gap: 1,
          }}
        >
          <Box
            ref={mainRef}
            sx={{
              width: '100%',
              flex: 1,
              minHeight: 0,
              display: 'grid',
              gridTemplateColumns: 'auto 1fr auto',
              gap: 2,
              alignItems: 'flex-start',
            }}
          >
            {/* 左: Threshold フェーダー */}
            <ParameterFader
              parameterId='THRESHOLD'
              label='THRESHOLD'
              sliderHeight={meterAndFaderHeight}
              min={-30}
              max={0}
              defaultValue={0}
              wheelStep={1}
              wheelStepFine={0.1}
              scaleMarks={[
                { value: 0, label: '0' },
                { value: -3, label: '-3' },
                { value: -6, label: '-6' },
                { value: -9, label: '-9' },
                { value: -12, label: '-12' },
                { value: -18, label: '-18' },
                { value: -24, label: '-24' },
                { value: -30, label: '-30' },
              ]}
            />

            {/* 中央: メーター群（モード別に出し分け）+ 下段にモード切替ボタン
                フェーダー側も各メーター側もヘッダを height: 36 に固定しているため、
                ここでは追加の mt を取らない（バー上端と rail 上端は自動で揃う）。 */}
            <Box sx={{ display: 'flex', flexDirection: 'column', alignItems: 'center' }}>
              {/* メーター 3 列を隣接させる（列幅を固定して左右にズレないように） */}
              <Box sx={{ display: 'flex', gap: 0.25, alignItems: 'flex-start' }}>
                {/* IN */}
                <Box sx={{ display: 'flex', flexDirection: 'column', alignItems: 'center', width: 52 }}>
                  {meterMode === 'momentary' ? (
                    <>
                      <LoudnessMeterBar lkfs={inLkfs} label='IN' />
                      <Tooltip title='Reset Hold'>
                        <Box
                        onClick={resetInHold}
                        sx={{
                          mt: 0.25,
                          display: 'flex',
                          alignItems: 'center',
                          justifyContent: 'center',
                          height: 14,
                          cursor: 'pointer',
                          userSelect: 'none',
                        }}
                      >
                          <Typography variant='caption' sx={{ fontSize: '10px', width: 52, textAlign: 'center', lineHeight: 1 }}>
                            {formatLkfs(inLkfsHold)}
                          </Typography>
                        </Box>
                      </Tooltip>
                    </>
                  ) : (
                    <>
                      {/* バーと "L IN R" ラベル行。IN は中央に絶対配置 */}
                      <Box sx={{ position: 'relative', display: 'flex', gap: 0.25 }}>
                        <LevelMeterBar level={inL} label='L' />
                        <LevelMeterBar level={inR} label='R' />
                        <Typography
                          sx={{
                            position: 'absolute',
                            // L/R ラベル（36px ヘッダ下端）のすぐ上に重ねる
                            top: '12px',
                            left: '50%',
                            transform: 'translateX(-50%)',
                            fontSize: '9px',
                            color: 'text.secondary',
                            fontWeight: 600,
                            lineHeight: 1,
                            pointerEvents: 'none',
                          }}
                        >
                          IN
                        </Typography>
                      </Box>
                      <Tooltip title='Reset Hold'>
                        <Box
                          onClick={resetInHold}
                          sx={{
                            mt: 0.25,
                            display: 'flex',
                            gap: 0.25,
                            alignItems: 'center',
                            justifyContent: 'center',
                            height: 14,
                            cursor: 'pointer',
                            userSelect: 'none',
                          }}
                        >
                          <Typography variant='caption' sx={{ fontSize: '10px', width: 24, textAlign: 'center', lineHeight: 1 }}>
                            {formatDb(inHold.left)}
                          </Typography>
                          <Typography variant='caption' sx={{ fontSize: '10px', width: 24, textAlign: 'center', lineHeight: 1 }}>
                            {formatDb(inHold.right)}
                          </Typography>
                        </Box>
                      </Tooltip>
                    </>
                  )}
                </Box>

                {/* GR */}
                <Box sx={{ display: 'flex', flexDirection: 'column', alignItems: 'center' }}>
                  <GainReductionMeterBar grDb={grDb} />
                  <Tooltip title='Reset Hold'>
                    <Box
                      onClick={resetGrHold}
                      sx={{
                        mt: 0.25,
                        display: 'flex',
                        alignItems: 'center',
                        justifyContent: 'center',
                        height: 14,
                        cursor: 'pointer',
                        userSelect: 'none',
                      }}
                    >
                      <Typography variant='caption' sx={{ fontSize: '10px', width: 48, textAlign: 'center', lineHeight: 1 }}>
                        -{grHold.toFixed(1)}
                      </Typography>
                    </Box>
                  </Tooltip>
                </Box>

                {/* OUT */}
                <Box sx={{ display: 'flex', flexDirection: 'column', alignItems: 'center', width: 52 }}>
                  {meterMode === 'momentary' ? (
                    <>
                      <LoudnessMeterBar lkfs={outLkfs} label='OUT' />
                      <Tooltip title='Reset Hold'>
                        <Box
                        onClick={resetOutHold}
                        sx={{
                          mt: 0.25,
                          display: 'flex',
                          alignItems: 'center',
                          justifyContent: 'center',
                          height: 14,
                          cursor: 'pointer',
                          userSelect: 'none',
                        }}
                      >
                          <Typography variant='caption' sx={{ fontSize: '10px', width: 52, textAlign: 'center', lineHeight: 1 }}>
                            {formatLkfs(outLkfsHold)}
                          </Typography>
                        </Box>
                      </Tooltip>
                    </>
                  ) : (
                    <>
                      <Box sx={{ position: 'relative', display: 'flex', gap: 0.25 }}>
                        <LevelMeterBar level={outL} label='L' />
                        <LevelMeterBar level={outR} label='R' />
                        <Typography
                          sx={{
                            position: 'absolute',
                            // L/R ラベル（36px ヘッダ下端）のすぐ上に重ねる
                            top: '12px',
                            left: '50%',
                            transform: 'translateX(-50%)',
                            fontSize: '9px',
                            color: 'text.secondary',
                            fontWeight: 600,
                            lineHeight: 1,
                            pointerEvents: 'none',
                          }}
                        >
                          OUT
                        </Typography>
                      </Box>
                      <Tooltip title='Reset Hold'>
                        <Box
                          onClick={resetOutHold}
                          sx={{
                            mt: 0.25,
                            display: 'flex',
                            gap: 0.25,
                            alignItems: 'center',
                            justifyContent: 'center',
                            height: 14,
                            cursor: 'pointer',
                            userSelect: 'none',
                          }}
                        >
                          <Typography variant='caption' sx={{ fontSize: '10px', width: 24, textAlign: 'center', lineHeight: 1 }}>
                            {formatDb(outHold.left)}
                          </Typography>
                          <Typography variant='caption' sx={{ fontSize: '10px', width: 24, textAlign: 'center', lineHeight: 1 }}>
                            {formatDb(outHold.right)}
                          </Typography>
                        </Box>
                      </Tooltip>
                    </>
                  )}
                </Box>
              </Box>

              {/* メーター群の下にモード切替ボタン（メーター列の幅に影響しない位置） */}
              <Tooltip title='Meter display mode' arrow>
                <Button
                  onClick={cycleMeterMode}
                  size='small'
                  variant='contained'
                  sx={{
                    mt: 1,
                    textTransform: 'none',
                    // "Momentary" も含めて固定幅にし、ラベル切替で横幅が動かないように。
                    width: 92,
                    minWidth: 92,
                    px: 1,
                    py: 0.2,
                    height: 24,
                    border: '2px solid',
                    borderColor: 'divider',
                    backgroundColor: 'transparent',
                    color: 'text.primary',
                    '&:hover': { backgroundColor: 'grey.700' },
                  }}
                  aria-label='meter display mode'
                >
                  {MODE_LABEL[meterMode]}
                </Button>
              </Tooltip>
            </Box>

            {/* 右: Output Gain フェーダー */}
            <ParameterFader
              parameterId='OUTPUT_GAIN'
              label='OUTPUT'
              sliderHeight={meterAndFaderHeight}
              min={-30}
              max={0}
              defaultValue={0}
              wheelStep={1}
              wheelStepFine={0.1}
              scaleMarks={[
                { value: 0, label: '0' },
                { value: -3, label: '-3' },
                { value: -6, label: '-6' },
                { value: -9, label: '-9' },
                { value: -12, label: '-12' },
                { value: -18, label: '-18' },
                { value: -24, label: '-24' },
                { value: -30, label: '-30' },
              ]}
            />
          </Box>

          {/* Release セクション（Auto Release + 手動 release 時定数） */}
          <ReleaseSection />
        </Paper>

        {/* リサイズハンドル */}
        <div
          onPointerDown={onDragStart}
          onPointerMove={onDrag}
          onPointerUp={onDragEnd}
          style={{
            position: 'fixed',
            right: 0,
            bottom: 0,
            width: 24,
            height: 24,
            cursor: 'nwse-resize',
            zIndex: 2147483647,
          }}
          title='Resize'
        />
      </Box>

      <LicenseDialog open={licenseOpen} onClose={closeLicenseDialog} />
      <GlobalDialog />
    </ThemeProvider>
  );
}

export default App;
