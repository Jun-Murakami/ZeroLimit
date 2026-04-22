import React, { useEffect, useRef, useState } from 'react';
import { Box, Input, Slider, Typography } from '@mui/material';
import { darken, lighten, styled } from '@mui/material/styles';
import { useJuceSliderValue } from '../hooks/useJuceParam';

interface ParameterFaderProps {
  /** JUCE パラメータID（例: 'THRESHOLD', 'OUTPUT_GAIN'） */
  parameterId: string;
  /** 値の表示レンジ下限（dB など実値スケール） */
  min: number;
  /** 値の表示レンジ上限 */
  max: number;
  /** ラベル */
  label: string;
  /** 目盛りに配置する実値の配列（下→上の順） */
  scaleMarks?: Array<{ value: number; label: string }>;
  /** Ctrl/Cmd + クリックで戻るデフォルト実値 */
  defaultValue?: number;
  /** マウスホイールの刻み（通常） */
  wheelStep?: number;
  /** マウスホイールの刻み（Shift 押下時） */
  wheelStepFine?: number;
  /** フェーダーのトラック色 */
  color?: 'primary' | 'secondary';
  active?: boolean;
  /** フェーダー本体の高さ（メーターと揃える用途で外から制御） */
  sliderHeight?: number;
}

const StyledSlider = styled(Slider)(({ theme }) => {
  const primaryMain = theme.palette.primary.main;
  const primaryLight = theme.palette.primary.light || lighten(primaryMain, 0.2);
  const primaryDark = theme.palette.primary.dark || darken(primaryMain, 0.2);
  const trackGradient = `linear-gradient(180deg, ${lighten(primaryLight, 0.15)} 0%, ${primaryMain} 50%, ${darken(
    primaryDark,
    0.15,
  )} 100%)`;
  const thumbTop = lighten(primaryMain, 0.9);
  const thumbMid1 = lighten(primaryMain, 0.6);
  const thumbMid2 = lighten(primaryMain, 0.2);
  const thumbBottom = darken(primaryMain, 0.2);
  const thumbGradient = `linear-gradient(180deg, ${thumbTop} 0%, ${thumbMid1} 40%, ${thumbMid2} 60%, ${thumbBottom} 100%)`;

  return {
    '& .MuiSlider-rail': {
      width: 8,
      borderRadius: 2,
      backgroundColor: '#1a1a1a',
      border: '1px solid #404040',
      opacity: 1,
    },
    '& .MuiSlider-track': {
      width: 8,
      borderRadius: 2,
      border: 'none',
      background: trackGradient,
    },
    '& .MuiSlider-thumb': {
      width: 20,
      height: 28,
      borderRadius: 4,
      background: thumbGradient,
      border: '1px solid rgba(0,0,0,0.35)',
      boxShadow: ['0 2px 4px rgba(0,0,0,0.45)', 'inset 0 1px 0 rgba(255,255,255,0.7)', 'inset 0 -2px 3px rgba(0,0,0,0.25)'].join(
        ', ',
      ),
      overflow: 'hidden',
      boxSizing: 'border-box',
      '&::before': {
        content: '""',
        position: 'absolute',
        left: 2,
        right: 2,
        top: 4,
        height: 9,
        borderRadius: 3,
        background: 'linear-gradient(180deg, rgba(255,255,255,0.85) 0%, rgba(255,255,255,0.35) 60%, rgba(255,255,255,0) 100%)',
        pointerEvents: 'none',
      },
      '&::after': {
        content: '""',
        position: 'absolute',
        left: '20%',
        right: '20%',
        top: '34%',
        bottom: '30%',
        borderRadius: 2,
        background:
          'repeating-linear-gradient(180deg, rgba(0,0,0,0.35) 0 1px, rgba(255,255,255,0.38) 1px 2px, rgba(0,0,0,0) 2px 6px)',
        boxShadow: '0 0 0 1px rgba(0,0,0,0.08) inset',
        pointerEvents: 'none',
      },
    },
  };
});

const StyledInput = styled(Input)(() => ({
  '& input': {
    padding: '2px 4px',
    fontSize: '10px',
    textAlign: 'center',
    width: '54px',
    backgroundColor: '#252525',
    color: 'text.primary',
    border: '1px solid #404040',
    borderRadius: 2,
    '&:focus': {
      borderColor: '#4fc3f7',
      backgroundColor: '#252525',
      outline: 'none',
    },
  },
  '&::before, &::after': {
    display: 'none',
  },
}));

// 実値 → 0..1 (線形) へ正規化
const toNorm = (value: number, min: number, max: number): number => {
  if (max === min) return 0;
  return Math.max(0, Math.min(1, (value - min) / (max - min)));
};
const fromNorm = (norm: number, min: number, max: number): number => min + (max - min) * Math.max(0, Math.min(1, norm));

export const ParameterFader: React.FC<ParameterFaderProps> = ({
  parameterId,
  min,
  max,
  label,
  scaleMarks,
  defaultValue,
  wheelStep = 1,
  wheelStepFine = 0.1,
  color = 'primary',
  active = true,
  sliderHeight,
}) => {
  const SLIDER_HEIGHT = sliderHeight ?? 160;

  // APVTS からのリアクティブ購読。JUCE の scaled 値がそのまま真のソース。
  const { value, state: sliderState, setScaled } = useJuceSliderValue(parameterId);

  // 編集中だけローカル保持する input テキスト。それ以外は value から導出。
  const [isEditing, setIsEditing] = useState(false);
  const [inputText, setInputText] = useState<string>('');
  const [isDragging, setIsDragging] = useState(false);

  // wheel ハンドラから最新値を参照するための Latest Ref Pattern
  const valueRef = useRef<number>(value);
  valueRef.current = value;

  const applyValue = (v: number) => {
    setScaled(v, min, max);
  };

  const displayInput = isEditing ? inputText : value.toFixed(1);

  const handleChange = (_: Event, v: number | number[]) => {
    const n = v as number; // 0..100
    applyValue(fromNorm(n / 100, min, max));
  };

  const handleInputChange = (e: React.ChangeEvent<HTMLInputElement>) => setInputText(e.target.value);
  const handleInputFocus = () => {
    setIsEditing(true);
    setInputText(valueRef.current.toFixed(1));
  };
  const commitInput = () => {
    setIsEditing(false);
    const parsed = parseFloat(inputText);
    if (!isNaN(parsed)) applyValue(parsed);
  };

  const handleInputKeyDown = (e: React.KeyboardEvent) => {
    if (e.key === 'Enter') (e.target as HTMLInputElement).blur();
  };

  const handleClickReset = (e: React.MouseEvent) => {
    if ((e.ctrlKey || e.metaKey) && defaultValue !== undefined) {
      e.preventDefault();
      e.stopPropagation();
      applyValue(defaultValue);
    }
  };

  // ホイール（非パッシブ）
  const wheelRef = useRef<HTMLDivElement | null>(null);
  useEffect(() => {
    const el = wheelRef.current;
    if (!el) return;
    const onWheel = (event: WheelEvent) => {
      event.preventDefault();
      const direction = -event.deltaY > 0 ? 1 : -1;
      const step = event.shiftKey ? wheelStepFine : wheelStep;
      applyValue(valueRef.current + step * direction);
    };
    el.addEventListener('wheel', onWheel, { passive: false });
    return () => {
      el.removeEventListener('wheel', onWheel as EventListener);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [min, max, wheelStep, wheelStepFine]);

  return (
    <Box sx={{ display: 'flex', flexDirection: 'column', alignItems: 'center', width: 60, position: 'relative' }}>
      {/* ラベルは固定高さ 36px のヘッダ領域に配置。
          - つまみ（高さ 28px）が最大位置で rail 上端を突き抜けても、この余白が確保されているため被らない。
          - メーター列も同じ 36px ヘッダを持たせることで、rail 上端とバー上端が自動的に揃う。
          目盛りラベル(scaleMarks)は inner wrapper 基準で絶対配置しているので、
          外枠幅を詰めても目盛りが右のグリッドギャップにオーバーハングするだけで見た目は保たれる。 */}
      <Box sx={{ height: 36, display: 'flex', alignItems: 'flex-start', justifyContent: 'center', width: '100%' }}>
        <Typography
          variant='caption'
          sx={{
            fontWeight: 600,
            fontSize: '0.68rem',
            color: active ? 'primary.main' : 'text.secondary',
            letterSpacing: '0.3px',
            lineHeight: 1,
          }}
        >
          {label}
        </Typography>
      </Box>

      <Box sx={{ display: 'flex', height: SLIDER_HEIGHT, width: '100%', justifyContent: 'center', mb: '14px' }}>
        <Box sx={{ position: 'relative', display: 'flex', alignItems: 'center' }} ref={wheelRef}>
          <StyledSlider
            value={toNorm(value, min, max) * 100}
            onChange={handleChange}
            onMouseDown={(e) => {
              handleClickReset(e);
              if (!e.defaultPrevented) {
                setIsDragging(true);
                sliderState?.sliderDragStarted();
              }
            }}
            onMouseUp={() => {
              if (isDragging) {
                setIsDragging(false);
                sliderState?.sliderDragEnded();
              }
            }}
            onChangeCommitted={() => {
              if (isDragging) {
                setIsDragging(false);
                sliderState?.sliderDragEnded();
              }
            }}
            min={0}
            max={100}
            step={0.1}
            orientation='vertical'
            sx={{ color: active ? color : 'grey.500', height: SLIDER_HEIGHT }}
          />

          {scaleMarks && scaleMarks.length > 0 && (
            <Box
              sx={{
                position: 'absolute',
                top: 0,
                left: 'calc(50% + 16px)',
                height: SLIDER_HEIGHT,
                display: 'flex',
                flexDirection: 'column',
                width: 28,
              }}
            >
              {scaleMarks.map((mark) => (
                <Typography
                  key={mark.value}
                  sx={{
                    position: 'absolute',
                    bottom: `${toNorm(mark.value, min, max) * 100}%`,
                    transform: 'translateY(50%)',
                    fontSize: '9px',
                    color: 'text.primary',
                    lineHeight: 1,
                    userSelect: 'none',
                    width: '100%',
                    textAlign: 'left',
                  }}
                >
                  {mark.label}
                </Typography>
              ))}
            </Box>
          )}
        </Box>
      </Box>

      {/* mt: '10px' — フェーダー側入力ボックスの上端を、メーター側モード切替ボタンの
          上端とほぼ同じ Y に合わせるための余白（slider の mb 14px 分だけでは足りない差分）。 */}
      <StyledInput
        className='block-host-shortcuts'
        value={displayInput}
        onChange={handleInputChange}
        onFocus={handleInputFocus}
        onBlur={commitInput}
        onKeyDown={handleInputKeyDown}
        disableUnderline
        sx={{ mt: '10px' }}
      />
    </Box>
  );
};
