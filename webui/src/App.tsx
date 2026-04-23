import { useEffect, useRef, useState } from 'react';
import { Box, Button, Paper, Tooltip, Typography } from '@mui/material';
import { CssBaseline, ThemeProvider } from '@mui/material';
import { juceBridge } from './bridge/juce';
import { useJuceComboBoxIndex, useJuceSliderState, useJuceToggleValue } from './hooks/useJuceParam';
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
import { useGlobalZoomGuard } from './hooks/useGlobalZoomGuard';
import { GlobalDialog } from './components/GlobalDialog';
import LicenseDialog from './components/LicenseDialog';
import { WebTransportBar } from './components/WebTransportBar';
import { WaveformView } from './components/WaveformView';
import { WebDemoMenu } from './components/WebDemoMenu';

const IS_WEB_MODE = import.meta.env.VITE_RUNTIME === 'web';
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
  useGlobalZoomGuard();

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

  // METERING_MODE パラメータ（APVTS 側）を useSyncExternalStore 経由で購読
  const { index: meterModeIndex, setIndex: setMeterModeIndexJuce } = useJuceComboBoxIndex('METERING_MODE');
  const meterMode: MeterMode = MODES[meterModeIndex] ?? 'peak';

  // DISPLAY_MODE（0 = Metering, 1 = Waveform）。中央のビジュアル切替。
  //  UI トグル本体は ReleaseSection 側（最上段右）。ここでは現在値だけ購読してレイアウト分岐に使う。
  const { index: displayModeIndex } = useJuceComboBoxIndex('DISPLAY_MODE');
  const isWaveformMode = displayModeIndex === 1;

  // モード切替時に古い state をリセットしてちらつきを防ぐ。
  //  JUCE は選択モードに該当するデータしか送らないため、切替直後の一瞬、
  //  UI は新モードで描画する一方で内部 state は旧モードの値のままになる。
  //  そのフレームで一時的に旧値が新モード UI 上に露出するのがちらつきの正体。
  //  切替ハンドラで新モード側の state を MIN に落としておけば、次の meterUpdate 到着で
  //  フレッシュな値に置き換わるまでの 1 フレームは "無音" 表示になって自然。
  const resetMetersForMode = (nextIndex: number) => {
    if (nextIndex === 2) {
      setInLkfs(MIN_LKFS);
      setOutLkfs(MIN_LKFS);
      setInLkfsHold(MIN_LKFS);
      setOutLkfsHold(MIN_LKFS);
    } else {
      setInL(MIN_DB); setInR(MIN_DB);
      setOutL(MIN_DB); setOutR(MIN_DB);
      setInHold({ left: MIN_DB, right: MIN_DB });
      setOutHold({ left: MIN_DB, right: MIN_DB });
    }
  };

  const cycleMeterMode = () => {
    const next = (meterModeIndex + 1) % MODES.length;
    resetMetersForMode(next);
    setMeterModeIndexJuce(next);
  };

  // Waveform モード時は METERING_MODE を Peak (=0) に固定する。
  //  実行は Waveform/Metering トグル（ReleaseSection 側）の onClick で
  //  DISPLAY_MODE と同じハンドラ内で同時更新する（useEffect で値変化を監視しない方針）。

  // ============================
  // Threshold / Output Gain の Link 機能
  // ============================
  //   - Link ON の瞬間の (Output - Threshold) のオフセットを記憶し、
  //     以降は片方を動かすともう片方も同じ delta だけ動く。
  //   - どちらかがレンジ端 (-30..0) にぶつかったら、そちらをクランプ。
  //     もう片方は継続可能（オフセット一時崩れ、戻れば回復）。
  //   - JUCE からのエコーで無限ループしないように mirroring 中フラグを見る。
  // Threshold / Output Gain は mirror 処理で state オブジェクトだけ必要なので、
  //  value の購読をしない useJuceSliderState を使う。これにより T/O の値変化
  //  （rapid drag 中は毎フレーム発生）で App が再レンダーされなくなり、
  //  Link ON 時の操作側フェーダーのワブリングが解消される。
  const thresholdSlider = useJuceSliderState('THRESHOLD');
  const outputGainSlider = useJuceSliderState('OUTPUT_GAIN');
  const { value: linkActive, setValue: setLinkJuce } = useJuceToggleValue('LINK');

  // Link state を listener クロージャから参照するための ref（Latest Ref Pattern）。
  //  useEffect で反映させると commit 後に更新されるため、commit より前に走る listener が
  //  1 フレーム古い値を見る timing bug がありうる。render 中代入で常に同期を保証する。
  const linkActiveRef = useRef<boolean>(linkActive);
  linkActiveRef.current = linkActive;

  // Link ON 時点の (Output - Threshold) オフセット
  const deltaRef = useRef<number>(0);

  // APVTS のレンジは THRESHOLD / OUTPUT_GAIN 共に -30..0 dB
  const PARAM_MIN_DB = -30;
  const PARAM_MAX_DB = 0;
  const clampParamDb = (db: number): number => Math.max(PARAM_MIN_DB, Math.min(PARAM_MAX_DB, db));
  const dbToNorm = (db: number): number => (clampParamDb(db) - PARAM_MIN_DB) / (PARAM_MAX_DB - PARAM_MIN_DB);

  // ループ止めの 2 軸:
  //   (1) idempotent: 書こうとしている値と相手の現在値が既に一致していればスキップ
  //       → 平常時の echo が綺麗にループを自然終端させる
  //   (2) suppress window: 書き込み直後の約 80ms は相手の listener を黙らせる
  //       → ユーザーが T を連続ドラッグしたとき、非同期で後から届く O echo が
  //          「過去の O 値」をベースに T を書き戻してしまう race を防ぐ
  const SYNC_TOLERANCE_DB = 0.05;
  const SUPPRESS_WINDOW_MS = 80;
  const suppressThresholdUntilRef = useRef<number>(0);
  const suppressOutputUntilRef    = useRef<number>(0);

  // LINK が OFF→ON に変わる瞬間を検出して delta を更新する（DAW オートメーション対応）。
  //  render 中に rising edge 検出 + ref 更新を行う。ユーザー操作は toggleLink() 側でも同じ
  //  delta を計算しているが（そちらは setState 前に ref を確定させる）、DAW オートメーション経由で
  //  外部から linkActive が変わるケースをここでカバーする。
  //  StrictMode で render が 2 回走っても、2 回目は prevRef=cur となり rising edge 検出は自然に無効化される。
  const prevLinkActiveRef = useRef<boolean>(linkActive);
  if (linkActive && ! prevLinkActiveRef.current) {
    const tNow = thresholdSlider ? thresholdSlider.getScaledValue() : 0;
    const oNow = outputGainSlider ? outputGainSlider.getScaledValue() : 0;
    deltaRef.current = oNow - tNow;
  }
  prevLinkActiveRef.current = linkActive;

  // Threshold 変化 → Output をミラー（必要な時のみ）
  useEffect(() => {
    if (!thresholdSlider || !outputGainSlider) return;
    const id = thresholdSlider.valueChangedEvent.addListener(() => {
      if (Date.now() < suppressThresholdUntilRef.current) return; // 自分で書いた echo は黙殺
      if (! linkActiveRef.current) return;
      const curT = thresholdSlider.getScaledValue();
      const curO = outputGainSlider.getScaledValue();
      const desiredO = clampParamDb(curT + deltaRef.current);
      if (Math.abs(curO - desiredO) < SYNC_TOLERANCE_DB) return;
      suppressOutputUntilRef.current = Date.now() + SUPPRESS_WINDOW_MS;
      outputGainSlider.setNormalisedValue(dbToNorm(desiredO));
    });
    return () => thresholdSlider.valueChangedEvent.removeListener(id);
  }, [thresholdSlider, outputGainSlider]);

  // Output 変化 → Threshold をミラー（必要な時のみ）
  useEffect(() => {
    if (!thresholdSlider || !outputGainSlider) return;
    const id = outputGainSlider.valueChangedEvent.addListener(() => {
      if (Date.now() < suppressOutputUntilRef.current) return;
      if (! linkActiveRef.current) return;
      const curO = outputGainSlider.getScaledValue();
      const curT = thresholdSlider.getScaledValue();
      const desiredT = clampParamDb(curO - deltaRef.current);
      if (Math.abs(curT - desiredT) < SYNC_TOLERANCE_DB) return;
      suppressThresholdUntilRef.current = Date.now() + SUPPRESS_WINDOW_MS;
      thresholdSlider.setNormalisedValue(dbToNorm(desiredT));
    });
    return () => outputGainSlider.valueChangedEvent.removeListener(id);
  }, [thresholdSlider, outputGainSlider]);

  const toggleLink = () => {
    const next = ! linkActive;
    if (next) {
      // setLinkJuce 呼び出しの前に delta を確定させておくことで、
      //  直後に飛んでくる valueChangedEvent に依存せず一貫した値になる
      const tNow = thresholdSlider ? thresholdSlider.getScaledValue() : 0;
      const oNow = outputGainSlider ? outputGainSlider.getScaledValue() : 0;
      deltaRef.current = oNow - tNow;
    }
    setLinkJuce(next);
  };

  // 直近の meteringMode を ref で保持。DAW オートメーション等で外部からモードが
  //  変わったとき、"新モード × 旧値" の 1 フレームを防ぐために値リセットを挟む。
  const lastMeterModeRef = useRef<number>(meterModeIndex);
  useEffect(() => {
    const id = juceBridge.addEventListener('meterUpdate', (d: unknown) => {
      const m = d as MeterUpdateData;
      const mode = typeof m.meteringMode === 'number' ? m.meteringMode : lastMeterModeRef.current;

      // モード変化を検出したら、その新モード側の state を一旦 MIN にしてからフレッシュ値を入れる。
      if (mode !== lastMeterModeRef.current) {
        lastMeterModeRef.current = mode;
        if (mode === 2) {
          setInLkfs(MIN_LKFS); setOutLkfs(MIN_LKFS);
          setInLkfsHold(MIN_LKFS); setOutLkfsHold(MIN_LKFS);
        } else {
          setInL(MIN_DB); setInR(MIN_DB); setOutL(MIN_DB); setOutR(MIN_DB);
          setInHold({ left: MIN_DB, right: MIN_DB });
          setOutHold({ left: MIN_DB, right: MIN_DB });
        }
      }

      if (mode === 2) {
        const iL = m.input?.momentary ?? MIN_LKFS;
        const oL = m.output?.momentary ?? MIN_LKFS;
        setInLkfs(iL);
        setOutLkfs(oL);
        setInLkfsHold((p) => (iL > p ? clampLkfs(iL) : p));
        setOutLkfsHold((p) => (oL > p ? clampLkfs(oL) : p));
      } else {
        const isRms = mode === 1;
        const iL = (isRms ? m.input?.rmsLeft  : m.input?.truePeakLeft)  ?? MIN_DB;
        const iR = (isRms ? m.input?.rmsRight : m.input?.truePeakRight) ?? MIN_DB;
        const oL = (isRms ? m.output?.rmsLeft  : m.output?.truePeakLeft)  ?? MIN_DB;
        const oR = (isRms ? m.output?.rmsRight : m.output?.truePeakRight) ?? MIN_DB;
        setInL(iL); setInR(iR); setOutL(oL); setOutR(oR);
        // hold 値は変化があった時のみ新オブジェクトを返して不要な再レンダーを抑える
        setInHold((p) => {
          const left = Math.max(p.left, clampDb(iL));
          const right = Math.max(p.right, clampDb(iR));
          return left === p.left && right === p.right ? p : { left, right };
        });
        setOutHold((p) => {
          const left = Math.max(p.left, clampDb(oL));
          const right = Math.max(p.right, clampDb(oR));
          return left === p.left && right === p.right ? p : { left, right };
        });
      }

      const gr = m.grDb ?? 0;
      setGrDb(gr);
      setGrHold((p) => (gr > p ? gr : p));
    });
    return () => juceBridge.removeEventListener(id);
  }, []);

  // ネイティブへの "ready" 通知（マウント時 1 回）。関連性のない contextmenu listener は別エフェクトに分離。
  useEffect(() => {
    juceBridge.whenReady(() => {
      juceBridge.callNative('system_action', 'ready');
    });
  }, []);

  // 右クリック (contextmenu) 抑制。入力系要素・明示 opt-in クラス・DEV モードは除外。
  useEffect(() => {
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
  //  加えて「リサイズ中」状態を debounce で検出 → WaveformView に伝えて描画を一時停止する
  //   （ウィンドウドラッグ中は ResizeObserver が 60Hz+ で連続発火し、canvas 再 alloc と
  //    ポリゴン描画が重なって重くなるため）。
  const mainRef = useRef<HTMLDivElement | null>(null);
  const [mainSize, setMainSize] = useState<{ width: number; height: number }>({ width: 520, height: 260 });
  const [isResizing, setIsResizing] = useState(false);
  useEffect(() => {
    const el = mainRef.current;
    if (!el) return;
    let debounceTimer: ReturnType<typeof setTimeout> | null = null;
    const ro = new ResizeObserver((entries) => {
      for (const entry of entries) {
        const w = entry.contentRect.width;
        const h = entry.contentRect.height;
        setMainSize((prev) => (prev.width !== w || prev.height !== h ? { width: w, height: h } : prev));
      }
      // 連続発火している間は true、最後の発火から 150ms 静寂になったら false に戻す
      setIsResizing(true);
      if (debounceTimer) clearTimeout(debounceTimer);
      debounceTimer = setTimeout(() => setIsResizing(false), 150);
    });
    ro.observe(el);
    return () => {
      ro.disconnect();
      if (debounceTimer) clearTimeout(debounceTimer);
    };
  }, []);

  // --- 派生サイズ ---
  //   フェーダー / メーターの縦長部分 (slider rail or canvas) に割り当てる高さ。
  //   メーター列のヘッダ(36) + hold 行(14 + mt 2) + モード切替ボタン(22 + mt 4) の 78px を差し引く。
  const meterAndFaderHeight = Math.max(80, Math.floor(mainSize.height - 78));

  //   中央メーター領域の幅 = 全体幅 - フェーダー幅×2 (60×2) - grid の gap 2(=16)×2 = 全体 -152
  //   そこから GR バー幅(48) と IN/OUT 間のセンターギャップ(0.25×2 = 4) を差し引いて 2 等分。
  const meterAreaWidth = Math.max(0, mainSize.width - 60 * 2 - 16 * 2);
  const meterColumnWidth = Math.max(52, Math.floor((meterAreaWidth - 48 - 4) / 2));
  //   L/R ペアの各バー幅（gap 0.25 = 2px を引いて半分）
  const levelBarWidth = Math.max(24, Math.floor((meterColumnWidth - 2) / 2));

  // Waveform モード時のレイアウト：
  //   [Waveform ... | 薄 GR | 薄 OUT(merged)]
  //   右側の薄いバー 2 本分と gap を確保し、残りを波形キャンバスに割り当てる。
  const thinBarWidth = 18;
  const thinGap = 2;
  const waveformWidth = Math.max(60, meterAreaWidth - thinBarWidth * 2 - thinGap * 2);

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
    // C++ 側 PluginEditor の kMinWidth / kMinHeight と合わせる
    const w = Math.max(447, dragState.current.startW + dx);
    const h = Math.max(390, dragState.current.startH + dy);
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
        /* 右下リサイズハンドルの視覚（ドット 3 つを斜めに並べる） */
        #resizeHandle::after {
          content: '';
          position: absolute;
          right: 4px;
          top: 8px;
          width: 2px;
          height: 2px;
          background: rgba(79, 195, 247, 1);
          border-radius: 1px;
          pointer-events: none;
          box-shadow:
            -4px 4px 0 0 rgba(79, 195, 247, 1),
            -8px 8px 0 0 rgba(79, 195, 247, 1),
            -1px 7px 0 0 rgba(79, 195, 247, 1);
        }

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
      <Box
        sx={IS_WEB_MODE
          ? {
              // Web デモ：ブラウザ中央にプラグインカードを配置する。
              minHeight: '100vh',
              display: 'flex',
              flexDirection: 'column',
              alignItems: 'center',
              justifyContent: 'center',
              py: 4,
              px: 2,
              gap: 1.5,
            }
          : {
              // プラグイン：DAW ウィンドウ全体を占有する。
              height: '100vh',
              display: 'flex',
              flexDirection: 'column',
              p: 2,
              pt: 0,
              overflow: 'hidden',
            }
        }
      >
        {/* Web モード時のみ、トランスポート（再生 / シーク / Bypass / ファイル選択）を
            プラグインカードの外に配置する。プラグイン本体の機能ではない操作系なので。 */}
        {IS_WEB_MODE && (
          <Box sx={{ width: '100%', maxWidth: 600 }}>
            <WebTransportBar />
          </Box>
        )}

        {/* Web モード時はプラグイン UI をカード化して幅固定・影つきに。
            プラグインモードでは透過（display: 'contents'）して従来のフレックス挙動を維持。
            最小幅は plugin 版の kMinWidth=447 に揃える（下段の Bands + トグルが収まる幅）。 */}
        <Box
          sx={IS_WEB_MODE
            ? {
                width: '100%',
                maxWidth: 600,
                height: 500,
                display: 'flex',
                flexDirection: 'column',
                overflow: 'hidden',
                p: 2,
                pt: 0,
                borderRadius: 2,
                boxShadow: 8,
                backgroundColor: 'background.default',
              }
            : { display: 'contents' }
          }
        >
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
            gap: 0,
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
              position: 'relative',
            }}
          >
            {/* Threshold と Output Gain の Link トグル。
                THRESHOLD / OUTPUT ラベルの行と同じ Y に、主画面の水平中央に配置する。 */}
            <Box
              sx={{
                position: 'absolute',
                top: 0,
                left: '50%',
                transform: 'translateX(-50%)',
                zIndex: 2,
                display: 'flex',
                alignItems: 'center',
                gap: 0.25,
                userSelect: 'none',
              }}
            >
              <Tooltip title='Link Threshold ⇔ Output Gain' arrow>
                <Button
                  onClick={toggleLink}
                  size='small'
                  variant='contained'
                  aria-pressed={linkActive}
                  sx={{
                    minWidth: 'auto',
                    px: 1,
                    py: 0.2,
                    height: 24,
                    textTransform: 'none',
                    letterSpacing: 0.5,
                    border: '2px solid',
                    borderColor: linkActive ? 'primary.main' : 'divider',
                    backgroundColor: linkActive ? 'primary.main' : 'transparent',
                    color: linkActive ? 'background.paper' : 'text.primary',
                    '&:hover': {
                      backgroundColor: linkActive ? 'primary.dark' : 'grey.700',
                    },
                  }}
                >
                  {'‹ Link ›'}
                </Button>
              </Tooltip>
            </Box>

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
              {isWaveformMode ? (
                // ---- Waveform モード: 左に波形キャンバス、右に細い GR + OUT(merged) ----
                <Box sx={{ display: 'flex', gap: `${thinGap}px`, alignItems: 'flex-start' }}>
                  {/* Waveform キャンバス（ヘッダ分 36px の空きをつくって他列とバー上端を揃える） */}
                  <Box sx={{ display: 'flex', flexDirection: 'column', alignItems: 'center' }}>
                    <Box sx={{ height: 36 }} />
                    <WaveformView width={waveformWidth} height={meterAndFaderHeight} isResizing={isResizing} />
                    {/* hold 行と高さを揃える空きボックス（モード切替でズレないため） */}
                    <Box sx={{ mt: 0.25, height: 14 }} />
                  </Box>

                  {/* 薄い GR */}
                  <Box sx={{ display: 'flex', flexDirection: 'column', alignItems: 'center' }}>
                    <GainReductionMeterBar grDb={grDb} width={thinBarWidth} height={meterAndFaderHeight} compact />
                    <Tooltip title='Reset Hold'>
                      <Box
                        onClick={resetGrHold}
                        sx={{ mt: 0.25, display: 'flex', alignItems: 'center', justifyContent: 'center', height: 14, cursor: 'pointer', userSelect: 'none' }}
                      >
                        <Typography variant='caption' sx={{ fontSize: '9px', width: thinBarWidth, textAlign: 'center', lineHeight: 1 }}>
                          -{grHold.toFixed(1)}
                        </Typography>
                      </Box>
                    </Tooltip>
                  </Box>

                  {/* 薄い OUT（merged: max(L,R) を単バーで） */}
                  <Box sx={{ display: 'flex', flexDirection: 'column', alignItems: 'center' }}>
                    <LevelMeterBar
                      level={Math.max(outL, outR)}
                      label='OUT'
                      width={thinBarWidth}
                      height={meterAndFaderHeight}
                    />
                    <Tooltip title='Reset Hold'>
                      <Box
                        onClick={resetOutHold}
                        sx={{ mt: 0.25, display: 'flex', alignItems: 'center', justifyContent: 'center', height: 14, cursor: 'pointer', userSelect: 'none' }}
                      >
                        <Typography variant='caption' sx={{ fontSize: '9px', width: thinBarWidth, textAlign: 'center', lineHeight: 1 }}>
                          {formatDb(Math.max(outHold.left, outHold.right))}
                        </Typography>
                      </Box>
                    </Tooltip>
                  </Box>
                </Box>
              ) : (
              // ---- Metering モード（従来のメーター 3 列） ----
              // メーター 3 列を隣接させる（列幅を固定して左右にズレないように）
              <Box sx={{ display: 'flex', gap: 0.25, alignItems: 'flex-start' }}>
                {/* IN */}
                <Box sx={{ display: 'flex', flexDirection: 'column', alignItems: 'center', width: meterColumnWidth }}>
                  {meterMode === 'momentary' ? (
                    <>
                      <LoudnessMeterBar lkfs={inLkfs} label='IN' width={meterColumnWidth} height={meterAndFaderHeight} />
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
                          <Typography variant='caption' sx={{ fontSize: '10px', width: meterColumnWidth, textAlign: 'center', lineHeight: 1 }}>
                            {formatLkfs(inLkfsHold)}
                          </Typography>
                        </Box>
                      </Tooltip>
                    </>
                  ) : (
                    <>
                      {/* バーと "L IN R" ラベル行。IN は中央に絶対配置 */}
                      <Box sx={{ position: 'relative', display: 'flex', gap: 0.25 }}>
                        <LevelMeterBar level={inL} label='L' width={levelBarWidth} height={meterAndFaderHeight} />
                        <LevelMeterBar level={inR} label='R' width={levelBarWidth} height={meterAndFaderHeight} />
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
                          <Typography variant='caption' sx={{ fontSize: '10px', width: levelBarWidth, textAlign: 'center', lineHeight: 1 }}>
                            {formatDb(inHold.left)}
                          </Typography>
                          <Typography variant='caption' sx={{ fontSize: '10px', width: levelBarWidth, textAlign: 'center', lineHeight: 1 }}>
                            {formatDb(inHold.right)}
                          </Typography>
                        </Box>
                      </Tooltip>
                    </>
                  )}
                </Box>

                {/* GR */}
                <Box sx={{ display: 'flex', flexDirection: 'column', alignItems: 'center' }}>
                  <GainReductionMeterBar grDb={grDb} height={meterAndFaderHeight} />
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
                <Box sx={{ display: 'flex', flexDirection: 'column', alignItems: 'center', width: meterColumnWidth }}>
                  {meterMode === 'momentary' ? (
                    <>
                      <LoudnessMeterBar lkfs={outLkfs} label='OUT' width={meterColumnWidth} height={meterAndFaderHeight} />
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
                          <Typography variant='caption' sx={{ fontSize: '10px', width: meterColumnWidth, textAlign: 'center', lineHeight: 1 }}>
                            {formatLkfs(outLkfsHold)}
                          </Typography>
                        </Box>
                      </Tooltip>
                    </>
                  ) : (
                    <>
                      <Box sx={{ position: 'relative', display: 'flex', gap: 0.25 }}>
                        <LevelMeterBar level={outL} label='L' width={levelBarWidth} height={meterAndFaderHeight} />
                        <LevelMeterBar level={outR} label='R' width={levelBarWidth} height={meterAndFaderHeight} />
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
                          <Typography variant='caption' sx={{ fontSize: '10px', width: levelBarWidth, textAlign: 'center', lineHeight: 1 }}>
                            {formatDb(outHold.left)}
                          </Typography>
                          <Typography variant='caption' sx={{ fontSize: '10px', width: levelBarWidth, textAlign: 'center', lineHeight: 1 }}>
                            {formatDb(outHold.right)}
                          </Typography>
                        </Box>
                      </Tooltip>
                    </>
                  )}
                </Box>
              </Box>
              )}

              {/* メーター群の下にモード切替ボタン（Peak / RMS / Momentary）。
                  Waveform/Metering 切替は Release セクション最上段の右側へ移動した。
                  Waveform モード時は右端の細い OUT バーしか出ず Peak 固定なので、ここも非表示。 */}
              {!isWaveformMode && (
                <Tooltip title='Meter display mode (Peak / RMS / Momentary)' arrow>
                  <Button
                    onClick={cycleMeterMode}
                    size='small'
                    variant='contained'
                    sx={{
                      mt: 0.5,
                      textTransform: 'none',
                      // "Momentary" も含めて固定幅にし、ラベル切替で横幅が動かないように。
                      width: 92,
                      minWidth: 92,
                      px: 1,
                      py: 0.1,
                      height: 16,
                      fontSize: '0.72rem',
                      border: '1px solid',
                      borderColor: 'divider',
                      backgroundColor: 'transparent',
                      color: 'text.secondary',
                      '&:hover': { backgroundColor: 'grey.700' },
                    }}
                    aria-label='meter display mode'
                  >
                    {MODE_LABEL[meterMode]}
                  </Button>
                </Tooltip>
              )}
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

        {/* リサイズハンドル（視覚は #resizeHandle::after で描画）。Web モードでは不要 */}
        {!IS_WEB_MODE && <div
          id='resizeHandle'
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
            backgroundColor: 'transparent',
          }}
          title='Resize'
        />}
        </Box>

        {/* Web モード：プラグインカード直下に簡単な説明を添える */}
        {IS_WEB_MODE && (
          <Typography
            variant='caption'
            color='text.secondary'
            sx={{ mt: 1, textAlign: 'center', maxWidth: 453, lineHeight: 1.8, px: 2 }}
          >
            A zero-latency brickwall limiter with multiband processing. DSP compiled to WebAssembly — running fully in your browser.
            <br />
            ゼロレイテンシー・ブロードキャスト／マスタリング用のリミッターを WebAssembly で動かしているデモ版です。
          </Typography>
        )}
      </Box>

      <LicenseDialog open={licenseOpen} onClose={closeLicenseDialog} />
      <GlobalDialog />

      {/* Web デモ時のみ右下にハンバーガーメニュー（Drawer 含めて条件付きマウント） */}
      {IS_WEB_MODE && <WebDemoMenu />}
    </ThemeProvider>
  );
}

export default App;
