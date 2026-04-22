import React, { useState, useEffect, useRef } from 'react';
import { Box, Slider, Input, Typography } from '@mui/material';
import { styled, lighten, darken } from '@mui/material/styles';
import { getSliderState } from 'juce-framework-frontend-mirror';

interface GainFaderProps {
  /** 直接バインドする JUCE パラメータID（例: 'HOST_GAIN', 'PLAYLIST_GAIN'） */
  parameterId: string;
  /** ドラッグ開始/終了（UI連携のため任意） */
  onDragStart?: () => void;
  onDragEnd?: () => void;
  /** ラベル表示 */
  label: string;
  color?: 'primary' | 'secondary';
  active?: boolean;
  /** Ctrl/Cmd+クリック時に戻す dB 値（-120..0） */
  defaultValue?: number;
  /** 下部ラベル（"HOST"/"PLAYLIST" 等）を表示するか（省スペースのため既定 false） */
  showLabel?: boolean;
}

// カスタムスタイルのスライダー
// テーマのプライマリカラーから動的にグラデーションを生成するスライダー
const StyledSlider = styled(Slider)(({ theme }) => {
  // プライマリのバリエーションを用意（上=明 / 中=標準 / 下=暗）
  const primaryMain = theme.palette.primary.main;
  const primaryLight = theme.palette.primary.light || lighten(primaryMain, 0.2);
  const primaryDark = theme.palette.primary.dark || darken(primaryMain, 0.2);

  // トラック用グラデーション（縦方向）
  const trackGradient = `linear-gradient(180deg, ${lighten(primaryLight, 0.15)} 0%, ${primaryMain} 50%, ${darken(
    primaryDark,
    0.15
  )} 100%)`;

  // サム用グラデーション（縦方向、プライマリに白を混ぜて樹脂感）
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
      // プライマリベースのグラデーション
      background: trackGradient,
    },
    '& .MuiSlider-thumb': {
      // サム本体サイズ（Cubase風にやや大きめで角を丸める）
      width: 20,
      height: 28, // さらに縦長に
      borderRadius: 4,
      // 樹脂/金属感のあるグラデーション（上: 明 / 中: 少し彩度 / 下: 影）
      background: thumbGradient,
      // 外枠のエッジを強調（薄めの輪郭）
      border: '1px solid rgba(0,0,0,0.35)',
      // 立体感のための外側ドロップシャドウ + 内側ハイライト/影
      boxShadow: [
        '0 2px 4px rgba(0,0,0,0.45)', // 外側影
        'inset 0 1px 0 rgba(255,255,255,0.7)', // 上部ハイライト
        'inset 0 -2px 3px rgba(0,0,0,0.25)', // 下部内側影
      ].join(', '),
      // 疑似要素の描画がサム外へはみ出ないよう明示的にクリップ
      overflow: 'hidden',
      boxSizing: 'border-box',

      // グルーブ（筋）と天面ハイライトのオーバーレイを疑似要素で描画
      '&::before': {
        // 天面の楕円ハイライト（上がより明るく見える演出）
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
        // 筋の集合
        content: '""',
        position: 'absolute',
        left: '20%', // さらに内側に寄せて外にはみ出さない
        right: '20%',
        top: '34%',
        bottom: '30%',
        borderRadius: 2,
        // 交互に暗→明→透明で細い筋を描く。範囲を絞って端のはみ出しを防止
        background:
          'repeating-linear-gradient(180deg, rgba(0,0,0,0.35) 0 1px, rgba(255,255,255,0.38) 1px 2px, rgba(0,0,0,0) 2px 6px)',
        boxShadow: '0 0 0 1px rgba(0,0,0,0.08) inset',
        pointerEvents: 'none',
      },
    },
  };
});

// スタイル付きInput
const StyledInput = styled(Input)(() => ({
  '& input': {
    padding: '2px 4px',
    fontSize: '10px',
    textAlign: 'center',
    width: '45px',
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

export const GainFader: React.FC<GainFaderProps> = ({
  parameterId,
  onDragStart,
  onDragEnd,
  label,
  color = 'primary',
  active = false,
  defaultValue = -120,
  showLabel = false,
}) => {
  // フェーダー全体の縦サイズ（px）。全体をコンパクトにするため約2/3へ縮小。
  // メーターの高さ(≈140px)と揃える。
  const SLIDER_HEIGHT = 140;
  // JUCE パラメータ（存在しないIDの場合は null）
  const sliderStateRef = useRef<ReturnType<typeof getSliderState> | null>(null);
  if (parameterId && sliderStateRef.current === null) {
    sliderStateRef.current = getSliderState(parameterId) || null;
  }

  // ローカル dB 値（-120..0）
  const [localValue, setLocalValue] = useState<number>(() => {
    const st = sliderStateRef.current;
    if (st) {
      const n = st.getNormalisedValue();
      return n <= 0 ? -120 : n * 120 - 120;
    }
    return -120;
  });
  const [inputValue, setInputValue] = useState<string>(localValue <= -120 ? '-∞' : localValue.toFixed(1));
  const [isDragging, setIsDragging] = useState(false);
  // ホイール操作で preventDefault を適法に行うため、ネイティブの非パッシブリスナーを使う
  // React の onWheel はブラウザによってパッシブ扱いになり preventDefault が効かない場合がある
  const wheelAreaRef = useRef<HTMLDivElement | null>(null);
  // ホイールハンドラで最新の値にアクセスするための参照
  const localValueRef = useRef<number>(localValue);
  useEffect(() => {
    localValueRef.current = localValue;
  }, [localValue]);

  // 外部からの値の更新を反映（ドラッグ中は無視）
  useEffect(() => {
    // JUCE 直接バインド時: 値変更イベントを購読してローカル値を同期
    const st = sliderStateRef.current;
    if (!st) return;
    const listenerId = st.valueChangedEvent.addListener(() => {
      if (isDragging) return; // ドラッグ中はエコーを無視
      const n = st.getNormalisedValue();
      const db = n <= 0 ? -120 : n * 120 - 120;
      setLocalValue(db);
      setInputValue(db <= -120 ? '-∞' : db.toFixed(1));
    });
    return () => {
      st.valueChangedEvent.removeListener(listenerId);
    };
  }, [parameterId, isDragging]);

  // フェーダーの対数カーブ設定
  // Cubaseに近い挙動を目指し、振幅aを指数でマッピングし
  // dB = 20*log10(a) で算出する。kは中央50%で-15dBになるよう導出した定数。
  // 導出: a(0.5) = 10^(-15/20) = (exp(k*0.5)-1)/(exp(k)-1) から k ≈ 3.064
  const FADER_DB_MIN = -120; // ボトムの下限
  const FADER_EXP_K = 3.064; // 中央-15dBを満たす指数係数
  const FADER_EXP_E = Math.exp(FADER_EXP_K); // 事前計算: exp(k)
  const FADER_DENOM = FADER_EXP_E - 1.0; // 正規化用の分母
  const SLIDER_ZERO_SNAP_PCT = 0.5; // この%以下は0に強制スナップ（-∞）

  // スライダー値（0-100）→ dB 変換
  const sliderToDb = (sliderValue: number): number => {
    // 0は -∞ とみなす（UI表示は-∞、内部は下限で打ち止め）
    if (sliderValue <= 0) return FADER_DB_MIN;
    if (sliderValue >= 100) return 0;

    // 0..1 に正規化
    const t = sliderValue / 100;
    // 指数カーブ: 振幅aは 0→1 に単調増加
    const amplitude = (Math.exp(FADER_EXP_K * t) - 1.0) / FADER_DENOM;
    // 数値誤差に対するガード
    const safeAmplitude = Math.max(0, Math.min(1, amplitude));
    // dBへ変換（下限をクリップ）
    const db = 20 * Math.log10(safeAmplitude);
    return Math.max(FADER_DB_MIN, Math.min(0, db));
  };

  // dB → スライダー値（0-100）変換（上記の逆変換）
  const dbToSlider = (db: number): number => {
    if (!isFinite(db) || db <= FADER_DB_MIN) return 0;
    if (db >= 0) return 100;

    // dB → 振幅（線形ゲイン）
    const amplitude = Math.pow(10, db / 20);
    // 逆変換: a = (exp(k t)-1)/(exp(k)-1) → exp(k t) = 1 + a*(exp(k)-1)
    const expkt = 1.0 + amplitude * FADER_DENOM;
    // 数値誤差を考慮して下限を確保
    const t = Math.log(Math.max(expkt, 1e-12)) / FADER_EXP_K;
    // 0..100へスケールしクリップ（丸めず連続値で返す）
    const slider = Math.max(0, Math.min(1, t)) * 100;
    return slider;
  };

  const handleSliderChange = (_: Event, value: number | number[]) => {
    const sliderValue = value as number;
    // 下端のわずかな残りで -60dB 付近に止まらないようスナップ
    const snapped = sliderValue <= SLIDER_ZERO_SNAP_PCT ? 0 : sliderValue;
    const db = sliderToDb(snapped);
    setLocalValue(db);
    setInputValue(db <= -120 ? '-∞' : db.toFixed(1));
    // dB → 正規化 0..1 へ変換し、JUCE へ反映
    const n = db <= -120 ? 0 : (db + 120) / 120;
    sliderStateRef.current?.setNormalisedValue(n);
  };

  const handleInputChange = (event: React.ChangeEvent<HTMLInputElement>) => {
    setInputValue(event.target.value);
  };

  const handleInputBlur = () => {
    let db = -120;
    if (inputValue === '-∞' || inputValue === '-inf') {
      db = -120;
    } else {
      const parsed = parseFloat(inputValue);
      if (!isNaN(parsed)) {
        db = Math.max(-120, Math.min(0, parsed));
      }
    }
    setLocalValue(db);
    setInputValue(db <= -120 ? '-∞' : db.toFixed(1));
    const n = db <= -120 ? 0 : (db + 120) / 120;
    sliderStateRef.current?.setNormalisedValue(n);
  };

  const handleInputKeyDown = (event: React.KeyboardEvent) => {
    if (event.key === 'Enter') {
      handleInputBlur();
    }
  };

  const handleDragStart = () => {
    setIsDragging(true);
    // JUCE へドラッグ開始を通知
    sliderStateRef.current?.sliderDragStarted();
    onDragStart?.();
  };

  const handleDragEnd = () => {
    // ドラッグを開始している場合のみ終了ジェスチャーを送る
    if (isDragging) {
      setIsDragging(false);
      // JUCE へドラッグ終了を通知
      sliderStateRef.current?.sliderDragEnded();
    }
    onDragEnd?.();
  };

  // Ctrl/Cmd+クリックでデフォルト値にリセット
  const handleSliderClick = (event: React.MouseEvent) => {
    if (event.ctrlKey || event.metaKey) {
      event.preventDefault();
      event.stopPropagation();
      setLocalValue(defaultValue);
      setInputValue(defaultValue <= -120 ? '-∞' : defaultValue.toFixed(1));
      const n = defaultValue <= -120 ? 0 : (defaultValue + 120) / 120;
      sliderStateRef.current?.setNormalisedValue(n);
    }
  };

  // ネイティブ wheel リスナー（passive: false）を登録して、スクロール抑止と細かなゲイン調整を実現
  useEffect(() => {
    const el = wheelAreaRef.current;
    if (!el) return;
    const handleWheelNative = (event: WheelEvent) => {
      // ブラウザスクロールを抑止（非パッシブリスナーのため有効）
      event.preventDefault();
      const current = localValueRef.current;
      // ホイール方向（上方向が正）
      const delta = -event.deltaY;
      // 調整量の決定
      let step: number;
      if (event.shiftKey) {
        // Shift+ホイール: 細かい調整（0.1dB）
        step = 0.1;
      } else if (current <= -30) {
        // -30dB 以下は大きめの刻み（10dB）で素早く移動
        step = 10;
      } else {
        // 通常: 標準調整（1dB）
        step = 1.0;
      }
      const direction = delta > 0 ? 1 : -1;
      const newValue = Math.max(-120, Math.min(0, current + step * direction));
      // UI と JUCE へ反映
      setLocalValue(newValue);
      setInputValue(newValue <= -120 ? '-∞' : newValue.toFixed(1));
      const n = newValue <= -120 ? 0 : (newValue + 120) / 120;
      sliderStateRef.current?.setNormalisedValue(n);
    };
    el.addEventListener('wheel', handleWheelNative, { passive: false });
    return () => {
      el.removeEventListener('wheel', handleWheelNative as EventListener);
    };
  }, [parameterId]);

  // 目盛りの値と位置（下から上へのdB値）
  const scaleMarks = [
    { value: 0, db: -120, label: '-oo' },
    { value: dbToSlider(-30), db: -30, label: '-30' },
    { value: dbToSlider(-24), db: -24, label: '-24' },
    { value: dbToSlider(-18), db: -18, label: '-18' },
    { value: dbToSlider(-12), db: -12, label: '-12' },
    { value: dbToSlider(-6), db: -6, label: '-6' },
    { value: 100, db: 0, label: '0' },
  ];

  return (
    <Box
      sx={{
        display: 'flex',
        flexDirection: 'column',
        alignItems: 'center',
        width: 60, // フェーダー中心と下要素の中央を合わせるため、列の幅をスリムに
        position: 'relative',
      }}
    >
      {/* スライダーと目盛り */}
      <Box
        sx={{
          display: 'flex',
          height: SLIDER_HEIGHT,
          position: 'relative',
          width: '100%',
          justifyContent: 'center', // フェーダーの軸を列の中央に
          mb: '14px', // サムが最下端で下ラベルに被らないよう下に余白を確保
        }}
      >
        {/* フェーダー本体 */}
        <Box sx={{ position: 'relative', display: 'flex', alignItems: 'center' }} ref={wheelAreaRef}>
          {/* フェーダースライダー */}
          <StyledSlider
            value={dbToSlider(localValue)}
            onChange={handleSliderChange}
            onMouseDown={(e) => {
              handleSliderClick(e);
              if (!e.defaultPrevented) {
                handleDragStart();
              }
            }}
            onMouseUp={handleDragEnd}
            onChangeCommitted={handleDragEnd}
            min={0}
            max={100}
            step={0.1}
            orientation='vertical'
            sx={{
              color: active ? color : 'grey.500',
              height: SLIDER_HEIGHT,
            }}
          />

          {/* 目盛り線は視認性とスペースのため非表示に変更 */}

          {/* 目盛りラベル（フェーダーのすぐ右） */}
          <Box
            sx={{
              position: 'absolute',
              top: 0,
              left: 'calc(50% + 16px)', // フェーダー中心(50%) + トラック半幅(4px) + 余白(6px)
              height: SLIDER_HEIGHT,
              display: 'flex',
              flexDirection: 'column',
              width: 24,
            }}
          >
            {scaleMarks.map((mark) => (
              <Typography
                key={mark.db}
                sx={{
                  position: 'absolute',
                  bottom: `${mark.value}%`,
                  transform: 'translateY(50%)',
                  fontSize: '9px',
                  color: 'contrastText',
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
        </Box>
      </Box>

      {/* ラベル（省スペース化のため既定は非表示） */}
      {showLabel && (
        <Typography
          variant='body2'
          sx={{
            mt: 1,
            fontWeight: 500,
            color: active ? '#4fc3f7' : 'contrastText',
            fontSize: '11px',
            userSelect: 'none',
            letterSpacing: '0.5px',
          }}
        >
          {label}
        </Typography>
      )}

      {/* 数値入力 */}
      <StyledInput
        className='block-host-shortcuts'
        value={inputValue}
        onChange={handleInputChange}
        onBlur={handleInputBlur}
        onKeyDown={handleInputKeyDown}
        disableUnderline
        sx={{ mt: 0.5 }}
      />
    </Box>
  );
};
