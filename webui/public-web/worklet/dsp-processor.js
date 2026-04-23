/**
 * ZeroLimit WASM AudioWorkletProcessor.
 * すべてのオーディオ処理（再生・リミッタ・メーター）は C++ WASM の dsp_process_block() に委譲。
 */

class DspProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.wasm = null;
    this.wasmReady = false;
    this.wasmMemory = null;

    // WASM ヒープ上のバッファポインタ
    this.outLPtr = 0;
    this.outRPtr = 0;
    this.meterBufPtr = 0; // 13 floats

    // 波形表示用（Pro-L 風オシロ）。ring から pull する JS 側のテンプバッファ。
    //  1 更新あたり最大でも数十サンプル（sampleRate / 200 × blocksPerUpdate）程度。
    //  余裕を持って 256 slot 用意。dsp_get_waveform_slices が書き込む。
    this.waveformMaxPerPull = 256;
    this.waveformPeaksPtr = 0;
    this.waveformGrDbPtr  = 0;
    // 波形機能が WASM 側にエクスポートされているか（後方互換）
    this.waveformAvailable = false;
    this.waveformSliceHz = 200;

    this.updateCounter = 0;

    this.port.onmessage = (e) => this.handleMessage(e.data);
  }

  handleMessage(msg) {
    switch (msg.type) {
      case 'init-wasm':
        this.initWasm(msg.wasmBytes);
        break;

      case 'load-source': {
        if (!this.wasm) break;
        const { left, right, numSamples, sourceSampleRate } = msg;
        const L = new Float32Array(left);
        const R = new Float32Array(right);

        const lPtr = this.wasm.dsp_alloc_buffer(numSamples);
        const rPtr = this.wasm.dsp_alloc_buffer(numSamples);
        const heap = new Float32Array(this.wasmMemory.buffer);
        heap.set(L, lPtr / 4);
        heap.set(R, rPtr / 4);

        this.wasm.dsp_load_source(lPtr, rPtr, numSamples, sourceSampleRate);

        this.wasm.dsp_free_buffer(lPtr);
        this.wasm.dsp_free_buffer(rPtr);
        break;
      }

      case 'clear-source':
        if (this.wasm) this.wasm.dsp_clear_source();
        break;

      case 'set-playing':
        if (this.wasm) this.wasm.dsp_set_playing(msg.value ? 1 : 0);
        break;

      case 'set-loop':
        if (this.wasm) this.wasm.dsp_set_loop(msg.value ? 1 : 0);
        break;

      case 'seek-normalised':
        if (this.wasm) this.wasm.dsp_seek_normalised(msg.value);
        break;

      case 'set-param': {
        if (!this.wasm) break;
        const p = msg.param;
        const v = msg.value;
        if (p === 'threshold_db')       this.wasm.dsp_set_threshold_db(v);
        else if (p === 'output_gain_db') this.wasm.dsp_set_output_gain_db(v);
        else if (p === 'release_ms')     this.wasm.dsp_set_release_ms(v);
        else if (p === 'auto_release')   this.wasm.dsp_set_auto_release(v ? 1 : 0);
        else if (p === 'mode')           this.wasm.dsp_set_mode(v);           // 0=Single, 1=Multi
        else if (p === 'band_count')     this.wasm.dsp_set_band_count(v);     // 3/4/5
        else if (p === 'metering_mode')  this.wasm.dsp_set_metering_mode(v);
        else if (p === 'bypass')         this.wasm.dsp_set_bypass(v ? 1 : 0);
        else if (p === 'reset_momentary') this.wasm.dsp_reset_momentary();
        break;
      }
    }
  }

  async initWasm(wasmBytes) {
    try {
      const module = await WebAssembly.compile(wasmBytes);
      const importObject = {
        env: { emscripten_notify_memory_growth: () => {} },
      };
      const instance = await WebAssembly.instantiate(module, importObject);
      if (instance.exports._initialize) instance.exports._initialize();

      this.wasm = instance.exports;
      this.wasmMemory = instance.exports.memory;

      // sampleRate はワークレットのグローバル定数
      this.wasm.dsp_init(sampleRate, 128);

      this.outLPtr = this.wasm.dsp_alloc_buffer(128);
      this.outRPtr = this.wasm.dsp_alloc_buffer(128);
      this.meterBufPtr = this.wasm.dsp_alloc_buffer(13);

      // 波形 API は古い wasm には無いので optional 扱い
      if (typeof this.wasm.dsp_get_waveform_slices === 'function') {
        this.waveformPeaksPtr = this.wasm.dsp_alloc_buffer(this.waveformMaxPerPull);
        this.waveformGrDbPtr  = this.wasm.dsp_alloc_buffer(this.waveformMaxPerPull);
        this.waveformAvailable = true;
        if (typeof this.wasm.dsp_get_waveform_slice_hz === 'function') {
          this.waveformSliceHz = this.wasm.dsp_get_waveform_slice_hz() || 200;
        }
      }

      this.wasmReady = true;
      this.port.postMessage({ type: 'wasm-ready' });
    } catch (err) {
      this.port.postMessage({ type: 'wasm-error', error: String(err) });
    }
  }

  process(inputs, outputs) {
    if (!this.wasmReady) return true;

    const output = outputs[0];
    if (!output || output.length < 2) return true;
    const outL = output[0];
    const outR = output[1];
    const n = outL.length;

    this.wasm.dsp_process_block(this.outLPtr, this.outRPtr, n);

    const heap = new Float32Array(this.wasmMemory.buffer);
    outL.set(heap.subarray(this.outLPtr / 4, this.outLPtr / 4 + n));
    outR.set(heap.subarray(this.outRPtr / 4, this.outRPtr / 4 + n));

    // ~20Hz でメインスレッドへ状態通知
    const interval = Math.max(1, Math.round(sampleRate / (n * 20)));
    if (++this.updateCounter >= interval) {
      this.updateCounter = 0;

      const stoppedAtEnd = this.wasm.dsp_consume_stopped_at_end();
      this.wasm.dsp_get_meter_data(this.meterBufPtr);
      const mh = new Float32Array(this.wasmMemory.buffer);
      const mo = this.meterBufPtr / 4;

      // 波形スライスをドレイン（optional）
      let waveformPayload = null;
      if (this.waveformAvailable) {
        const got = this.wasm.dsp_get_waveform_slices(
          this.waveformPeaksPtr,
          this.waveformGrDbPtr,
          this.waveformMaxPerPull,
        );
        if (got > 0) {
          const peaksView = new Float32Array(this.wasmMemory.buffer, this.waveformPeaksPtr, got);
          const grDbView  = new Float32Array(this.wasmMemory.buffer, this.waveformGrDbPtr,  got);
          // port に渡す時点で配列コピー（detach 不要の single-transferable 方針は見送り、
          //  postMessage の structured clone でコピーされる。slice 数は 1 更新あたり高々数十なので低負荷）
          waveformPayload = {
            sliceHz: this.waveformSliceHz,
            peaks:   Array.from(peaksView),
            grDb:    Array.from(grDbView),
          };
        }
      }

      this.port.postMessage({
        type: 'state-update',
        position: this.wasm.dsp_get_position(),
        duration: this.wasm.dsp_get_duration(),
        isPlaying: !!this.wasm.dsp_is_playing(),
        stoppedAtEnd: !!stoppedAtEnd,
        meter: {
          mode:           mh[mo + 0],
          inPeakLeft:     mh[mo + 1],
          inPeakRight:    mh[mo + 2],
          inRmsLeft:      mh[mo + 3],
          inRmsRight:     mh[mo + 4],
          inMomentary:    mh[mo + 5],
          outPeakLeft:    mh[mo + 6],
          outPeakRight:   mh[mo + 7],
          outRmsLeft:     mh[mo + 8],
          outRmsRight:    mh[mo + 9],
          outMomentary:   mh[mo + 10],
          grDb:           mh[mo + 11],
        },
        waveform: waveformPayload,
      });
    }

    return true;
  }
}

registerProcessor('dsp-processor', DspProcessor);
