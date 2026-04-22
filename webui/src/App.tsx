import { useCallback, useEffect, useRef, useState } from 'react';
import { Box, Paper, Typography } from '@mui/material';
import { CssBaseline, ThemeProvider } from '@mui/material';
import { juceBridge } from './bridge/juce';
import { darkTheme } from './theme';
import { ParameterFader } from './components/ParameterFader';
import { GainReductionMeterBar, LevelMeterBar, formatDb } from './components/VUMeter';
import { useHostShortcutForwarding } from './hooks/useHostShortcutForwarding';
import { GlobalDialog } from './components/GlobalDialog';
import LicenseDialog from './components/LicenseDialog';
import type { MeterUpdateData } from './types';
import './App.css';

const MIN_DB = -60;

function App() {
  useHostShortcutForwarding();

  const [inL, setInL] = useState(MIN_DB);
  const [inR, setInR] = useState(MIN_DB);
  const [outL, setOutL] = useState(MIN_DB);
  const [outR, setOutR] = useState(MIN_DB);
  const [grDb, setGrDb] = useState(0);

  // ピークホールド表示
  const [inPeak, setInPeak] = useState({ left: MIN_DB, right: MIN_DB });
  const [outPeak, setOutPeak] = useState({ left: MIN_DB, right: MIN_DB });
  const [grPeak, setGrPeak] = useState(0);

  const clamp = (db: number) => Math.max(MIN_DB, Math.min(0, db));

  useEffect(() => {
    const id = juceBridge.addEventListener('meterUpdate', (d: unknown) => {
      const m = d as MeterUpdateData;
      const il = m.input?.truePeakLeft ?? MIN_DB;
      const ir = m.input?.truePeakRight ?? MIN_DB;
      const ol = m.output?.truePeakLeft ?? MIN_DB;
      const or = m.output?.truePeakRight ?? MIN_DB;
      const gr = m.grDb ?? 0;
      setInL(il);
      setInR(ir);
      setOutL(ol);
      setOutR(or);
      setGrDb(gr);
      setInPeak((p) => ({ left: Math.max(p.left, clamp(il)), right: Math.max(p.right, clamp(ir)) }));
      setOutPeak((p) => ({ left: Math.max(p.left, clamp(ol)), right: Math.max(p.right, clamp(or)) }));
      setGrPeak((p) => Math.max(p, gr));
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

  const resetInPeak = useCallback(() => setInPeak({ left: MIN_DB, right: MIN_DB }), []);
  const resetOutPeak = useCallback(() => setOutPeak({ left: MIN_DB, right: MIN_DB }), []);
  const resetGrPeak = useCallback(() => setGrPeak(0), []);

  const [licenseOpen, setLicenseOpen] = useState(false);
  const openLicenseDialog = () => setLicenseOpen(true);
  const closeLicenseDialog = () => setLicenseOpen(false);

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

        <Paper elevation={2} sx={{ p: 2, mb: 1, flex: 1, minHeight: 0, display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
          <Box sx={{ display: 'grid', gridTemplateColumns: 'auto 1fr auto', gap: 2, alignItems: 'flex-start' }}>
            {/* 左: Threshold フェーダー */}
            <ParameterFader
              parameterId='THRESHOLD'
              label='THRESHOLD'
              min={-40}
              max={0}
              unit='dB'
              defaultValue={-1}
              wheelStep={1}
              wheelStepFine={0.1}
              scaleMarks={[
                { value: 0, label: '0' },
                { value: -6, label: '-6' },
                { value: -12, label: '-12' },
                { value: -18, label: '-18' },
                { value: -24, label: '-24' },
                { value: -30, label: '-30' },
                { value: -40, label: '-40' },
              ]}
            />

            {/* 中央: メーター群（IN L/R, GR, OUT L/R） */}
            <Box sx={{ display: 'flex', gap: 0.5, justifyContent: 'center', alignItems: 'flex-start' }}>
              <Box sx={{ display: 'flex', flexDirection: 'column', alignItems: 'center' }}>
                <Box sx={{ display: 'flex', gap: 0.25 }}>
                  <LevelMeterBar level={inL} label='IN L' />
                  <LevelMeterBar level={inR} label='IN R' />
                </Box>
                <Box
                  onClick={resetInPeak}
                  sx={{ mt: 0.25, display: 'flex', gap: 0.25, cursor: 'pointer', userSelect: 'none' }}
                  title='Reset Hold'
                >
                  <Typography variant='caption' sx={{ fontSize: '10px', width: 24, textAlign: 'center' }}>
                    {formatDb(inPeak.left)}
                  </Typography>
                  <Typography variant='caption' sx={{ fontSize: '10px', width: 24, textAlign: 'center' }}>
                    {formatDb(inPeak.right)}
                  </Typography>
                </Box>
              </Box>

              <Box sx={{ display: 'flex', flexDirection: 'column', alignItems: 'center' }}>
                <GainReductionMeterBar grDb={grDb} />
                <Box onClick={resetGrPeak} sx={{ mt: 0.25, cursor: 'pointer' }} title='Reset Hold'>
                  <Typography variant='caption' sx={{ fontSize: '10px', width: 48, textAlign: 'center' }}>
                    -{grPeak.toFixed(1)}
                  </Typography>
                </Box>
              </Box>

              <Box sx={{ display: 'flex', flexDirection: 'column', alignItems: 'center' }}>
                <Box sx={{ display: 'flex', gap: 0.25 }}>
                  <LevelMeterBar level={outL} label='OUT L' />
                  <LevelMeterBar level={outR} label='OUT R' />
                </Box>
                <Box
                  onClick={resetOutPeak}
                  sx={{ mt: 0.25, display: 'flex', gap: 0.25, cursor: 'pointer', userSelect: 'none' }}
                  title='Reset Hold'
                >
                  <Typography variant='caption' sx={{ fontSize: '10px', width: 24, textAlign: 'center' }}>
                    {formatDb(outPeak.left)}
                  </Typography>
                  <Typography variant='caption' sx={{ fontSize: '10px', width: 24, textAlign: 'center' }}>
                    {formatDb(outPeak.right)}
                  </Typography>
                </Box>
              </Box>
            </Box>

            {/* 右: Output Gain フェーダー */}
            <ParameterFader
              parameterId='OUTPUT_GAIN'
              label='OUTPUT'
              min={-24}
              max={24}
              unit='dB'
              defaultValue={0}
              wheelStep={1}
              wheelStepFine={0.1}
              scaleMarks={[
                { value: 24, label: '+24' },
                { value: 12, label: '+12' },
                { value: 6, label: '+6' },
                { value: 0, label: '0' },
                { value: -6, label: '-6' },
                { value: -12, label: '-12' },
                { value: -24, label: '-24' },
              ]}
            />
          </Box>
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
