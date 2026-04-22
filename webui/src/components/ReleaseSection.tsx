import React, { useEffect, useRef, useState } from 'react';
import { Box, FormControlLabel, Slider, Switch, Typography, Input } from '@mui/material';
import { useJuceSliderValue, useJuceToggleValue } from '../hooks/useJuceParam';

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
  const { value: releaseMs, state: sliderState, setNormalised } = useJuceSliderValue('RELEASE_MS');
  const { value: autoRelease, setValue: setAutoReleaseJuce } = useJuceToggleValue('AUTO_RELEASE', true);

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

  // Ctrl/Cmd クリックで既定値 1.0 ms
  const handleSliderClick = (e: React.MouseEvent) => {
    if (e.ctrlKey || e.metaKey) {
      e.preventDefault();
      e.stopPropagation();
      applyNormalised(msToNorm(1.0));
    }
  };

  const handleToggleChange = (e: React.ChangeEvent<HTMLInputElement>) => {
    setAutoReleaseJuce(e.target.checked);
  };

  // ホイール（非パッシブ）
  const wheelRef = useRef<HTMLDivElement | null>(null);
  useEffect(() => {
    const el = wheelRef.current;
    if (!el) return;
    const onWheel = (event: WheelEvent) => {
      event.preventDefault();
      const direction = -event.deltaY > 0 ? 1 : -1;
      const step = event.shiftKey ? 0.01 : 0.05;
      const currentT = msToNorm(releaseMsRef.current);
      applyNormalised(currentT + step * direction);
    };
    el.addEventListener('wheel', onWheel, { passive: false });
    return () => {
      el.removeEventListener('wheel', onWheel as EventListener);
    };
  }, [sliderState]);

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

  return (
    <Box
      sx={{
        width: '100%',
        mt: 1.5,
        border: '1px solid',
        borderColor: 'text.secondary',
        borderRadius: 1,
        p: 1,
      }}
    >
      <Box sx={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
        <FormControlLabel
          control={<Switch checked={autoRelease} onChange={handleToggleChange} size='small' />}
          label={autoRelease ? 'Auto Release' : 'Manual Release'}
          sx={{
            m: 0,
            color: autoRelease ? 'text.primary' : 'text.secondary',
            '& .MuiFormControlLabel-label': { fontSize: '0.875rem' },
          }}
        />
        <Box sx={{ display: 'flex', alignItems: 'center' }}>
          <Input
            className='block-host-shortcuts'
            value={displayInput}
            onChange={handleInputChange}
            onBlur={commitInput}
            onFocus={handleInputFocus}
            onKeyDown={handleInputKeyDown}
            size='small'
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
      <Box sx={{ px: 1, opacity: autoRelease ? 0.5 : 1.0, transition: 'opacity 120ms' }} ref={wheelRef}>
        <Slider
          value={sliderValue}
          onChange={handleSliderChange}
          onMouseDown={(e) => {
            if (e.ctrlKey || e.metaKey) {
              handleSliderClick(e);
              return;
            }
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
    </Box>
  );
};
