import React, { useEffect, useRef, useState } from 'react';
import { Box, FormControlLabel, Slider, Switch, Typography, Input } from '@mui/material';
import { getSliderState, getToggleState } from 'juce-framework-frontend-mirror';

//
//  Release セクション（Auto/Manual Release + 0.01..1000 ms 対数スライダー）
//
//  実装メモ：
//   - 状態は releaseMs 一本に絞る。スライダーの値は毎レンダーで msToNorm(releaseMs) を計算して渡す。
//   - JUCE の valueChangedEvent は自分の setNormalisedValue からも跳ね返ってくるため、
//     自送信直後の ~150ms はエコーとみなして state を書き戻さない（`lastEchoAt` で判定）。
//     これを怠ると：
//       a) ドラッグ中に thumb が往復して激しくちらつく
//       b) JUCE 側の浮動小数誤差で来る値が元の t と微妙にズレ、2 回目以降の grab で
//          "MUI が見る value" と "実画面上の thumb 位置" がずれて飛ぶ
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

// エコー抑制: 自分が setNormalisedValue した直後のこの期間は listener の state 書き戻しを無視する
const ECHO_WINDOW_MS = 150;

export const ReleaseSection: React.FC = () => {
  const sliderState = getSliderState('RELEASE_MS');
  const toggleState = getToggleState('AUTO_RELEASE');

  const initialMs = sliderState ? sliderState.getScaledValue() : 1.0;
  const [releaseMs, setReleaseMs] = useState<number>(initialMs);
  const [autoRelease, setAutoRelease] = useState<boolean>(() => (toggleState ? toggleState.getValue() : true));

  const [isEditing, setIsEditing] = useState(false);
  const [inputValue, setInputValue] = useState<string>('');
  const [isDragging, setIsDragging] = useState(false);

  // エコー抑制用タイムスタンプ
  const suppressUntilRef = useRef<number>(0);

  // JUCE → WebUI の値更新購読（外部変更のみ反映）
  useEffect(() => {
    if (!sliderState) return;
    const id = sliderState.valueChangedEvent.addListener(() => {
      if (Date.now() < suppressUntilRef.current) return; // 自送信のエコーは無視
      setReleaseMs(sliderState.getScaledValue());
    });
    return () => sliderState.valueChangedEvent.removeListener(id);
  }, [sliderState]);

  useEffect(() => {
    if (!toggleState) return;
    const id = toggleState.valueChangedEvent.addListener(() => {
      setAutoRelease(toggleState.getValue());
    });
    return () => toggleState.valueChangedEvent.removeListener(id);
  }, [toggleState]);

  // 表示用 input の同期
  useEffect(() => {
    if (!isEditing) setInputValue(formatMs(releaseMs).replace(' ms', '').trim());
  }, [releaseMs, isEditing]);

  const releaseMsRef = useRef<number>(releaseMs);
  useEffect(() => {
    releaseMsRef.current = releaseMs;
  }, [releaseMs]);

  // 値の書き込み（ローカル state + JUCE へ同期）。
  //  - WebUI → JUCE の方向では、直後にエコーで戻ってくる自分の値で state が上書きされないよう
  //    suppressUntil をセット。
  const applyNormalised = (t: number) => {
    const clampedT = Math.max(0, Math.min(1, t));
    const ms = normToMs(clampedT);
    suppressUntilRef.current = Date.now() + ECHO_WINDOW_MS;
    setReleaseMs(ms);
    sliderState?.setNormalisedValue(clampedT);
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
    const v = e.target.checked;
    setAutoRelease(v);
    toggleState?.setValue(v);
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

  const handleInputChange = (e: React.ChangeEvent<HTMLInputElement>) => setInputValue(e.target.value);
  const commitInput = () => {
    setIsEditing(false);
    const parsed = parseFloat(inputValue);
    if (!isNaN(parsed) && parsed > 0) applyNormalised(msToNorm(parsed));
  };
  const handleInputKeyDown = (e: React.KeyboardEvent<HTMLInputElement>) => {
    if (e.key === 'Enter') (e.target as HTMLInputElement).blur();
  };
  const handleInputFocus = () => {
    setIsEditing(true);
    setInputValue(formatMs(releaseMs).replace(' ms', '').trim());
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
            value={inputValue}
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
