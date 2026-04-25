import React, { useEffect, useRef, useState } from 'react';
import { Box, Divider, FormControlLabel, Slider, Switch, Tooltip, Typography, Input, useMediaQuery } from '@mui/material';
import { useJuceComboBoxIndex, useJuceSliderValue, useJuceToggleValue } from '../hooks/useJuceParam';
import { useFineAdjustPointer } from '../hooks/useFineAdjustPointer';
import { useNumberInputAdjust } from '../hooks/useNumberInputAdjust';

//
//  Release セクション（Auto/Manual Release + 0.01..1000 ms 対数スライダー）
//
//  状態の持ち方：
//   - `releaseMs` は JUCE APVTS から useSyncExternalStore 経由でリアクティブに取得。
//     自前 state は持たない。
//   - スライダー値は render 時に msToNorm(releaseMs) を都度計算して MUI Slider に渡す。
//   - input 表示値も render 時に formatMs(releaseMs) で導出（編集中は入力中文字列を優先）。
//

const formatMs = (ms: number): string => {
  if (ms < 0.1) return `${ms.toFixed(3)} ms`;
  if (ms < 10) return `${ms.toFixed(2)} ms`;
  if (ms < 100) return `${ms.toFixed(1)} ms`;
  return `${Math.round(ms)} ms`;
};

const MS_MIN = 0.01;
const MS_MAX = 1000;
const LOG_RATIO = Math.log(MS_MAX / MS_MIN);
const msToNorm = (ms: number): number => {
  const clamped = Math.max(MS_MIN, Math.min(MS_MAX, ms));
  return Math.log(clamped / MS_MIN) / LOG_RATIO;
};
const normToMs = (t: number): number => {
  const clamped = Math.max(0, Math.min(1, t));
  return MS_MIN * Math.pow(MS_MAX / MS_MIN, clamped);
};

export const ReleaseSection: React.FC = () => {
  // 狭い viewport（web デモをスマホ等 / プラグインを縮めた場合）ではトップ行に
  //  Multi-band + Bands + Waveform/Metering トグルが詰まるので縦スタックに切り替える。
  //  しきい値 440px。プラグインの最小幅 kMinWidth=340 では常に narrow 側に倒れる。
  //  narrow 時のレイアウト（公式仕様）:
  //    [上] Metering/Waveform トグル（中央寄せ）
  //    [中] Single/Multi-band スイッチ + Bands セレクタ（横並び 1 行）
  //    [下] Release セクション
  const isNarrow = useMediaQuery('(max-width: 440px)');

  const { value: releaseMs, state: sliderState, setNormalised } = useJuceSliderValue('RELEASE_MS');
  const { value: autoRelease, setValue: setAutoReleaseJuce } = useJuceToggleValue('AUTO_RELEASE', true);
  // Single / Multi バンドモード。Multi 時はこのセクション全体が効かなくなる（強制 Auto Release）。
  const { index: modeIndex, setIndex: setModeIndex } = useJuceComboBoxIndex('MODE');
  const multiMode = modeIndex === 1;
  // Multi モードのバンド数（0=3, 1=4, 2=5）
  const { index: bandCountIdx, setIndex: setBandCountIdx } = useJuceComboBoxIndex('BAND_COUNT');
  // DISPLAY_MODE（0=Metering, 1=Waveform）。Waveform/Metering トグル本体はこのセクションの右上。
  const { index: displayModeIdx, setIndex: setDisplayModeIdx } = useJuceComboBoxIndex('DISPLAY_MODE');
  const isWaveformMode = displayModeIdx === 1;
  // Waveform モードに入るときは METERING_MODE も Peak(=0) に強制したいので setter を取っておく。
  //  （value は使わないが、useJuceComboBoxIndex は購読もするので再レンダーは走る。
  //   ReleaseSection は METERING_MODE 値に依存した描画を持たないため影響なし。）
  const { setIndex: setMeterModeIdx } = useJuceComboBoxIndex('METERING_MODE');

  // Waveform / Metering トグルのハンドラ。
  //  Waveform へ入るときは METERING_MODE も Peak(=0) にリセット（細い OUT バーが Peak 固定なので）。
  //  meterUpdate 側でモード変化を検出して state をクリアする既存ロジックが走るため、
  //  こちらでは APVTS の 2 値を同時に書けば十分。
  const toggleDisplayMode = () => {
    const next = isWaveformMode ? 0 : 1;
    setDisplayModeIdx(next);
    if (next === 1) setMeterModeIdx(0);
  };

  const [isEditing, setIsEditing] = useState(false);
  const [inputText, setInputText] = useState<string>('');
  const [isDragging, setIsDragging] = useState(false);

  // 最新値への参照（wheel native listener でのみ使う）
  const releaseMsRef = useRef<number>(releaseMs);
  releaseMsRef.current = releaseMs; // render で参照を同期（useEffect 不要）

  // 書き込み: 正規化 0..1 を受けて JUCE に反映。
  //  ここで渡す t は「我々の log スケール上の 0..1」なので、frontend-mirror に
  //  そのまま setNormalisedValue させると値がずれる。
  //  frontend-mirror は lambda 形式の NormalisableRange を認識できず、skew=1 の
  //  線形換算しか行わない（juce-framework-frontend-mirror の normalisedToScaledValue 参照）。
  //  そこで一旦 ms に変換 → 線形 [0,1] に戻す → setNormalisedValue に渡す。
  //  こうすると frontend-mirror が線形で計算した scaled 値 = 我々の ms になり、
  //  C++ 側のパラメータ（log NormalisableRange）と同じ値で握手できる。
  const applyNormalised = (t: number) => {
    const clampedT = Math.max(0, Math.min(1, t));
    const ms = normToMs(clampedT);
    const linearT = (ms - MS_MIN) / (MS_MAX - MS_MIN);
    setNormalised(Math.max(0, Math.min(1, linearT)));
  };

  const handleSliderChange = (_: Event, value: number | number[]) => {
    applyNormalised(value as number);
  };

  const handleSliderCommitted = () => {
    if (isDragging) {
      setIsDragging(false);
      sliderState?.sliderDragEnded();
    }
  };

  const handleToggleChange = (e: React.ChangeEvent<HTMLInputElement>) => {
    setAutoReleaseJuce(e.target.checked);
  };

  // 修飾キー + ポインタ操作：
  //  Ctrl/Cmd + クリック      → 既定値 1.0 ms へリセット
  //  (Ctrl/Cmd/Shift) + ドラッグ → 微調整モード（log 正規化空間で 1px = 0.002 norm）
  //  修飾キーなし              → MUI Slider の通常ドラッグに委譲
  const fineDragStartNormRef = useRef<number>(0);
  const handleSliderPointerDownCapture = useFineAdjustPointer({
    orientation: 'horizontal',
    onReset: () => applyNormalised(msToNorm(1.0)),
    onDragStart: () => {
      fineDragStartNormRef.current = msToNorm(releaseMsRef.current);
      sliderState?.sliderDragStarted();
    },
    onDragDelta: (deltaPx) => {
      // 1px = 0.002 norm。log 軸全域（0.01..1000 ms）を 500px で横断。
      applyNormalised(fineDragStartNormRef.current + deltaPx * 0.002);
    },
    onDragEnd: () => sliderState?.sliderDragEnded(),
  });

  // ホイール（非パッシブ）
  const wheelRef = useRef<HTMLDivElement | null>(null);
  useEffect(() => {
    const el = wheelRef.current;
    if (!el) return;
    const onWheel = (event: WheelEvent) => {
      event.preventDefault();
      const direction = -event.deltaY > 0 ? 1 : -1;
      const fine = event.shiftKey || event.ctrlKey || event.metaKey || event.altKey;
      const step = fine ? 0.01 : 0.05;
      const currentT = msToNorm(releaseMsRef.current);
      applyNormalised(currentT + step * direction);
    };
    el.addEventListener('wheel', onWheel, { passive: false });
    return () => {
      el.removeEventListener('wheel', onWheel as EventListener);
    };
  }, [sliderState]);

  // 数値入力欄（ms）のホイール / 縦ドラッグ
  const inputElRef = useRef<HTMLInputElement | null>(null);
  const inputDragStartNormRef = useRef<number>(0);
  useNumberInputAdjust(inputElRef, {
    onWheelStep: (direction, fine) => {
      const step = fine ? 0.01 : 0.05;
      const currentT = msToNorm(releaseMsRef.current);
      applyNormalised(currentT + step * direction);
    },
    onDragStart: () => {
      inputDragStartNormRef.current = msToNorm(releaseMsRef.current);
      sliderState?.sliderDragStarted();
    },
    onDragDelta: (deltaY, fine) => {
      const step = fine ? 0.002 : 0.01;
      applyNormalised(inputDragStartNormRef.current + deltaY * step);
    },
    onDragEnd: () => sliderState?.sliderDragEnded(),
  });

  // Input: 表示値は編集中だけローカル state、それ以外は releaseMs から導出
  const displayInput = isEditing ? inputText : formatMs(releaseMs).replace(' ms', '').trim();
  const handleInputChange = (e: React.ChangeEvent<HTMLInputElement>) => setInputText(e.target.value);
  const commitInput = () => {
    setIsEditing(false);
    const parsed = parseFloat(inputText);
    if (!isNaN(parsed) && parsed > 0) applyNormalised(msToNorm(parsed));
  };
  const handleInputKeyDown = (e: React.KeyboardEvent<HTMLInputElement>) => {
    if (e.key === 'Enter') (e.target as HTMLInputElement).blur();
  };
  const handleInputFocus = () => {
    setIsEditing(true);
    setInputText(formatMs(releaseMs).replace(' ms', '').trim());
  };

  const sliderValue = msToNorm(releaseMs);

  // Multi モード時は Release セクション全体を半透明化して操作不能にする。
  //  （Auto Release は DSP 側で強制 ON、バンドごとに最適化された時定数が使われる）
  const releaseSectionOpacity = multiMode ? 0.4 : 1.0;
  const releaseSectionDisabled = multiMode;

  return (
    // レイアウト方針（T 字型 divider）:
    //   外枠 = flex column
    //    ├── 上段: 左右 2 カラム flex（中央に垂直 Divider、上段の高さまで）
    //    ├── 水平 Divider（外枠の全幅: 左端〜右端）
    //    └── 下段: 全幅（Auto/Manual + ms + Slider）
    //   これにより 3 つの交点（水平 divider の左端 / 中央 T / 右端）がすべて外枠に接触する。
    //   外枠直下の p:1 は削除し、各コンテンツボックスへ個別にパディングをかけることで
    //   divider は外枠まで届きつつ、文字要素のインセットは従来どおり保つ。
    <Box
      sx={{
        width: '100%',
        border: '1px solid',
        borderColor: 'divider',
        borderRadius: 1,
        display: 'flex',
        flexDirection: 'column',
      }}
    >
      {/* ====== 上段: バンドモード切替 + 表示モード切替 ======
          各バンド数の crossover とバンド別時定数は DSP 側で固定（ゼロコンフィグ）:
            3-band: 120 Hz / 5 kHz                 （放送、声を Mid に閉じ込め）← 既定
            4-band: 150 Hz / 5 kHz / 15 kHz        （Steinberg 準拠）
            5-band: 80 / 250 / 1k / 5k Hz          （UA 準拠、音楽マスタリング志向）
          narrow 時は Metering/Waveform を最上段センタリング、Single/Multi を 2 段（Switch + Bands）に分離。 */}
      {(() => {
        const singleMultiSwitch = (
          <FormControlLabel
            control={
              <Switch
                checked={multiMode}
                onChange={(e) => setModeIndex(e.target.checked ? 1 : 0)}
                size='small'
              />
            }
            label={multiMode ? 'Multi-band' : 'Single-band'}
            sx={{
              m: 0,
              color: multiMode ? 'primary.main' : 'text.primary',
              '& .MuiFormControlLabel-label': { fontSize: '0.875rem', fontWeight: multiMode ? 600 : 400 },
            }}
          />
        );

        const bandsBlock = multiMode ? (
          <Box sx={{ display: 'flex', alignItems: 'center', gap: 0.5 }}>
            {/* 「Bands」ラベル。数字だけだと何の値か不明瞭なので明示する。 */}
            <Typography variant='caption' sx={{ fontSize: '0.75rem', color: 'text.secondary', mr: 0.5 }}>
              Bands
            </Typography>
            {/* バンド数切替（3 / 4 / 5） */}
            <Box sx={{ display: 'flex', border: '1px solid', borderColor: 'divider', borderRadius: 1, overflow: 'hidden' }}>
              {[
                { label: '3', idx: 0 },
                { label: '4', idx: 1 },
                { label: '5', idx: 2 },
              ].map((opt) => {
                const active = bandCountIdx === opt.idx;
                return (
                  <Box
                    key={opt.idx}
                    onClick={() => setBandCountIdx(opt.idx)}
                    sx={{
                      px: 1,
                      py: 0.15,
                      fontSize: '0.72rem',
                      fontWeight: active ? 600 : 400,
                      cursor: 'pointer',
                      backgroundColor: active ? 'primary.main' : 'transparent',
                      color: active ? 'background.paper' : 'text.secondary',
                      minWidth: 22,
                      textAlign: 'center',
                      userSelect: 'none',
                      transition: 'background-color 80ms',
                      '&:hover': { backgroundColor: active ? 'primary.dark' : 'grey.700' },
                    }}
                  >
                    {opt.label}
                  </Box>
                );
              })}
            </Box>
          </Box>
        ) : null;

        const displayModeToggle = (
          <Tooltip title='Display: Metering (meters) ⇔ Waveform (oscilloscope)' arrow>
            <Box
              onClick={toggleDisplayMode}
              role='button'
              aria-label='display mode'
              sx={{
                display: 'inline-flex',
                height: 22,
                borderRadius: 1,
                border: '1px solid',
                borderColor: 'divider',
                overflow: 'hidden',
                cursor: 'pointer',
                userSelect: 'none',
                fontSize: '0.7rem',
                lineHeight: 1,
              }}
            >
              <Box
                sx={{
                  px: 0.75,
                  display: 'flex',
                  alignItems: 'center',
                  backgroundColor: !isWaveformMode ? 'primary.main' : 'transparent',
                  color: !isWaveformMode ? 'background.paper' : 'text.secondary',
                }}
              >
                Metering
              </Box>
              <Box
                sx={{
                  px: 0.75,
                  display: 'flex',
                  alignItems: 'center',
                  backgroundColor: isWaveformMode ? 'primary.main' : 'transparent',
                  color: isWaveformMode ? 'background.paper' : 'text.secondary',
                }}
              >
                Waveform
              </Box>
            </Box>
          </Tooltip>
        );

        if (isNarrow) {
          // narrow: [Display モード（センタリング）] / [Single/Multi + Bands（横並び 1 行）]
          return (
            <>
              <Box sx={{ p: 1, display: 'flex', justifyContent: 'center' }}>
                {displayModeToggle}
              </Box>
              <Divider />
              <Box sx={{ p: 1, display: 'flex', alignItems: 'center', gap: 1 }}>
                {singleMultiSwitch}
                {bandsBlock}
              </Box>
            </>
          );
        }

        // wide: [Single/Multi + Bands] | [Display モード]
        return (
          <Box sx={{ display: 'flex', flexDirection: 'row', alignItems: 'stretch' }}>
            <Box sx={{ flex: 1, minWidth: 0, p: 1, display: 'flex', alignItems: 'center', gap: 1 }}>
              {singleMultiSwitch}
              {bandsBlock}
            </Box>
            <Divider orientation='vertical' flexItem />
            <Box sx={{ p: 1, display: 'flex', alignItems: 'flex-start' }}>
              {displayModeToggle}
            </Box>
          </Box>
        );
      })()}

      {/* ====== 水平 Divider（外枠の全幅: 左端〜右端まで届く） ====== */}
      <Divider />

      {/* ====== 下段: Release（Auto/Manual + 時定数）======
          Single-band 時のみ機能。Multi 時はバンド別に最適化された Auto Release が
          DSP 側で強制されるため、ここは半透明 + 操作不可で無効化する。 */}
      <Box sx={{ p: 1 }}>
      <Box
        sx={{
          display: 'flex',
          justifyContent: 'space-between',
          alignItems: 'center',
          opacity: releaseSectionOpacity,
          transition: 'opacity 120ms',
          pointerEvents: releaseSectionDisabled ? 'none' : 'auto',
        }}
      >
        <FormControlLabel
          control={<Switch checked={autoRelease} onChange={handleToggleChange} size='small' disabled={releaseSectionDisabled} />}
          label={multiMode ? 'Auto (Multi-band)' : (autoRelease ? 'Auto Release' : 'Manual Release')}
          sx={{
            m: 0,
            color: autoRelease || multiMode ? 'text.primary' : 'text.secondary',
            '& .MuiFormControlLabel-label': { fontSize: '0.875rem' },
          }}
        />
        <Box sx={{ display: 'flex', alignItems: 'center' }}>
          <Input
            className='block-host-shortcuts'
            inputRef={inputElRef}
            value={displayInput}
            onChange={handleInputChange}
            onBlur={commitInput}
            onFocus={handleInputFocus}
            onKeyDown={handleInputKeyDown}
            size='small'
            disabled={releaseSectionDisabled}
            sx={{
              width: 64,
              fontFamily: '"Red Hat Mono", monospace',
              fontSize: '0.875rem',
              '& input': { textAlign: 'right', padding: '2px 4px' },
              '&:before': { borderBottom: 'none' },
              '&:hover:not(.Mui-disabled):before': { borderBottom: '1px solid rgba(255, 255, 255, 0.42)' },
              '&:after': { borderBottom: '1px solid', borderColor: 'primary.main' },
            }}
          />
          <Typography variant='body2' sx={{ ml: 0.5 }}>
            ms
          </Typography>
        </Box>
      </Box>
      <Box
        sx={{
          px: 1,
          opacity: (autoRelease || multiMode) ? 0.5 * releaseSectionOpacity : 1.0 * releaseSectionOpacity,
          transition: 'opacity 120ms',
          pointerEvents: releaseSectionDisabled ? 'none' : 'auto',
        }}
        ref={wheelRef}
        onPointerDownCapture={handleSliderPointerDownCapture}
      >
        <Slider
          value={sliderValue}
          onChange={handleSliderChange}
          onMouseDown={() => {
            if (!isDragging) {
              setIsDragging(true);
              sliderState?.sliderDragStarted();
            }
          }}
          onChangeCommitted={handleSliderCommitted}
          min={0}
          max={1}
          step={0.001}
          valueLabelDisplay='off'
          sx={{
            mb: -0.9,
            height: 6,
            '& .MuiSlider-thumb': {
              width: 12,
              height: 12,
              transition: 'opacity 80ms',
              opacity: 0,
            },
            '&:hover .MuiSlider-thumb, & .MuiSlider-thumb.Mui-focusVisible, & .MuiSlider-thumb.Mui-active': {
              opacity: 1,
            },
            '& .MuiSlider-track': { height: 3, transition: 'none' },
            '& .MuiSlider-rail': { height: 3, opacity: 0.5 },
            '& .MuiSlider-markLabel': { fontSize: '0.7rem', mt: -1 },
          }}
          marks={[
            { value: 0.0, label: '0.01' },
            { value: 0.2, label: '0.1' },
            { value: 0.4, label: '1' },
            { value: 0.6, label: '10' },
            { value: 0.8, label: '100' },
            { value: 1.0, label: '1000' },
          ]}
        />
      </Box>
      </Box>{/* /下段 p:1 wrapper */}
    </Box>
  );
};
