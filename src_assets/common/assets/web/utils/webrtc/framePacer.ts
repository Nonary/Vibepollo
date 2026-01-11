/**
 * FramePacer - Swap-paced renderer with strict 2-frame cap
 *
 * Uses "swap pacing" not "sleep pacing":
 * - Draw every RAF (always render the front buffer)
 * - Only control whether to swap in a new frame
 * - This avoids timer jitter and makes stutters measurable as long frames
 *
 * Rule set:
 * 1. Draw every RAF
 * 2. When a new decoded frame arrives (via rVFC), upload to pending slot
 * 3. Keep only 2 slots (front and pending). If new arrives while pending exists, overwrite pending
 * 4. On RAF, if pending exists, swap it to front exactly once, then draw front
 */

export interface FramePacerMetrics {
  /** Frames uploaded via rVFC */
  framesUploaded: number;
  /** Frames actually drawn to canvas (should match RAF count) */
  framesDrawn: number;
  /** Frames swapped from pending to front */
  framesSwapped: number;
  /** Frames skipped because a newer frame arrived before swap */
  framesSkipped: number;
  /** Last interval between rVFC callbacks in ms */
  lastRvfcIntervalMs: number | null;
  /** Last interval between rAF draws in ms */
  lastRafIntervalMs: number | null;
  /** Running average rVFC interval */
  avgRvfcIntervalMs: number | null;
  /** Running average rAF interval */
  avgRafIntervalMs: number | null;
  /** Soft hitches: RAF intervals > 20ms */
  softHitchCount: number;
  /** Hard hitches: RAF intervals > 33.4ms (missed frame at 60Hz) */
  hardHitchCount: number;
  /** p95 of RAF interval */
  p95RafIntervalMs: number | null;
  /** p99 of RAF interval */
  p99RafIntervalMs: number | null;
}

export interface FramePacerCallbacks {
  onMetrics?: (metrics: FramePacerMetrics) => void;
  onError?: (error: Error) => void;
}

export interface FramePacerOptions {
  /** Enable diagnostic metrics collection (default: true) */
  diagnosticsEnabled?: boolean;
}

/** Soft hitch threshold: > 20ms between frames */
const SOFT_HITCH_THRESHOLD_MS = 20;
/** Hard hitch threshold: > 33.4ms (missed frame at 60Hz) */
const HARD_HITCH_THRESHOLD_MS = 33.4;
/** Window size for percentile calculations */
const PERCENTILE_WINDOW_SIZE = 300;

/**
 * Pre-allocated texture slot for zero-allocation frame handling
 */
interface TextureSlot {
  texture: WebGLTexture;
  frameId: number;
  width: number;
  height: number;
  valid: boolean;
}

export class FramePacer {
  private video: HTMLVideoElement;
  private canvas: HTMLCanvasElement;
  private gl: WebGLRenderingContext | null = null;
  private program: WebGLProgram | null = null;
  private positionBuffer: WebGLBuffer | null = null;
  private texCoordBuffer: WebGLBuffer | null = null;

  // Cached attribute locations (avoid per-frame lookups)
  private positionLoc = -1;
  private texCoordLoc = -1;

  // 2-slot buffer: front (displayed) and pending (newest uploaded)
  private front: TextureSlot | null = null;
  private pending: TextureSlot | null = null;

  // Frame IDs for tracking
  private uploadFrameId = 0;
  private swappedFrameId = 0;

  // Handles for cleanup
  private rvfcHandle = 0;
  private rafHandle = 0;
  private running = false;

  // Metrics tracking (allocation-free after init)
  private metrics: FramePacerMetrics = {
    framesUploaded: 0,
    framesDrawn: 0,
    framesSwapped: 0,
    framesSkipped: 0,
    lastRvfcIntervalMs: null,
    lastRafIntervalMs: null,
    avgRvfcIntervalMs: null,
    avgRafIntervalMs: null,
    softHitchCount: 0,
    hardHitchCount: 0,
    p95RafIntervalMs: null,
    p99RafIntervalMs: null,
  };

  // Timing for intervals
  private lastRvfcTime = 0;
  private lastRafTime = 0;
  private rvfcIntervalSum = 0;
  private rvfcIntervalCount = 0;
  private rafIntervalSum = 0;
  private rafIntervalCount = 0;

  // Sliding window for percentile calculations (pre-allocated)
  private rafIntervalWindow: Float32Array;
  private rafIntervalWindowIndex = 0;
  private rafIntervalWindowFilled = 0;

  private callbacks: FramePacerCallbacks;

  // Uniform locations (cached to avoid per-frame lookups)
  private textureLoc: WebGLUniformLocation | null = null;

  // Diagnostics toggle - when false, skip all metrics collection
  private _diagnosticsEnabled = true;

  constructor(
    video: HTMLVideoElement,
    canvas: HTMLCanvasElement,
    callbacks: FramePacerCallbacks = {},
    options: FramePacerOptions = {},
  ) {
    this.video = video;
    this.canvas = canvas;
    this.callbacks = callbacks;
    this._diagnosticsEnabled = options.diagnosticsEnabled ?? true;
    // Pre-allocate percentile window
    this.rafIntervalWindow = new Float32Array(PERCENTILE_WINDOW_SIZE);
  }

  /**
   * Enable or disable diagnostics collection at runtime.
   * When disabled, only core swap-pacing runs (no metrics overhead).
   */
  set diagnosticsEnabled(enabled: boolean) {
    this._diagnosticsEnabled = enabled;
  }

  get diagnosticsEnabled(): boolean {
    return this._diagnosticsEnabled;
  }

  /**
   * Initialize WebGL context and resources.
   * Call once before start().
   */
  init(): boolean {
    try {
      const gl = this.canvas.getContext('webgl', {
        alpha: false,
        antialias: false,
        depth: false,
        stencil: false,
        preserveDrawingBuffer: false,
        powerPreference: 'high-performance',
        desynchronized: true, // Reduce compositor latency
      });

      if (!gl) {
        this.callbacks.onError?.(new Error('WebGL not available'));
        return false;
      }

      this.gl = gl;

      // Create shader program
      const vertSrc = `
        attribute vec2 a_position;
        attribute vec2 a_texCoord;
        varying vec2 v_texCoord;
        void main() {
          gl_Position = vec4(a_position, 0.0, 1.0);
          v_texCoord = a_texCoord;
        }
      `;

      const fragSrc = `
        precision mediump float;
        uniform sampler2D u_texture;
        varying vec2 v_texCoord;
        void main() {
          gl_FragColor = texture2D(u_texture, v_texCoord);
        }
      `;

      const vertShader = this.compileShader(gl, gl.VERTEX_SHADER, vertSrc);
      const fragShader = this.compileShader(gl, gl.FRAGMENT_SHADER, fragSrc);
      if (!vertShader || !fragShader) return false;

      const program = gl.createProgram();
      if (!program) {
        this.callbacks.onError?.(new Error('Failed to create program'));
        return false;
      }

      gl.attachShader(program, vertShader);
      gl.attachShader(program, fragShader);
      gl.linkProgram(program);

      if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
        this.callbacks.onError?.(new Error('Shader link failed'));
        return false;
      }

      this.program = program;

      // Cache attribute and uniform locations
      this.positionLoc = gl.getAttribLocation(program, 'a_position');
      this.texCoordLoc = gl.getAttribLocation(program, 'a_texCoord');
      this.textureLoc = gl.getUniformLocation(program, 'u_texture');

      // Create position buffer (fullscreen quad)
      this.positionBuffer = gl.createBuffer();
      gl.bindBuffer(gl.ARRAY_BUFFER, this.positionBuffer);
      // Two triangles covering clip space [-1, 1]
      gl.bufferData(
        gl.ARRAY_BUFFER,
        new Float32Array([
          -1, -1,
           1, -1,
          -1,  1,
          -1,  1,
           1, -1,
           1,  1,
        ]),
        gl.STATIC_DRAW,
      );

      // Create texcoord buffer
      this.texCoordBuffer = gl.createBuffer();
      gl.bindBuffer(gl.ARRAY_BUFFER, this.texCoordBuffer);
      // Flip Y for video
      gl.bufferData(
        gl.ARRAY_BUFFER,
        new Float32Array([
          0, 1,
          1, 1,
          0, 0,
          0, 0,
          1, 1,
          1, 0,
        ]),
        gl.STATIC_DRAW,
      );

      // Pre-allocate texture slots
      this.front = this.createTextureSlot(gl);
      this.pending = this.createTextureSlot(gl);

      if (!this.front || !this.pending) {
        this.callbacks.onError?.(new Error('Failed to create texture slots'));
        return false;
      }

      // Set up GL state once (doesn't change)
      gl.useProgram(program);
      gl.activeTexture(gl.TEXTURE0);
      gl.uniform1i(this.textureLoc, 0);

      // Bind and enable position attribute
      gl.bindBuffer(gl.ARRAY_BUFFER, this.positionBuffer);
      gl.enableVertexAttribArray(this.positionLoc);
      gl.vertexAttribPointer(this.positionLoc, 2, gl.FLOAT, false, 0, 0);

      // Bind and enable texcoord attribute
      gl.bindBuffer(gl.ARRAY_BUFFER, this.texCoordBuffer);
      gl.enableVertexAttribArray(this.texCoordLoc);
      gl.vertexAttribPointer(this.texCoordLoc, 2, gl.FLOAT, false, 0, 0);

      return true;
    } catch (e) {
      this.callbacks.onError?.(e instanceof Error ? e : new Error(String(e)));
      return false;
    }
  }

  private compileShader(
    gl: WebGLRenderingContext,
    type: number,
    source: string,
  ): WebGLShader | null {
    const shader = gl.createShader(type);
    if (!shader) return null;
    gl.shaderSource(shader, source);
    gl.compileShader(shader);
    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
      gl.deleteShader(shader);
      return null;
    }
    return shader;
  }

  private createTextureSlot(gl: WebGLRenderingContext): TextureSlot | null {
    const texture = gl.createTexture();
    if (!texture) return null;

    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);

    return {
      texture,
      frameId: -1,
      width: 0,
      height: 0,
      valid: false,
    };
  }

  /**
   * Start the frame pacing loop.
   * Requires init() to have been called first.
   */
  start(): void {
    if (this.running || !this.gl) return;
    this.running = true;

    // Reset metrics
    this.metrics = {
      framesUploaded: 0,
      framesDrawn: 0,
      framesSwapped: 0,
      framesSkipped: 0,
      lastRvfcIntervalMs: null,
      lastRafIntervalMs: null,
      avgRvfcIntervalMs: null,
      avgRafIntervalMs: null,
      softHitchCount: 0,
      hardHitchCount: 0,
      p95RafIntervalMs: null,
      p99RafIntervalMs: null,
    };
    this.lastRvfcTime = 0;
    this.lastRafTime = 0;
    this.rvfcIntervalSum = 0;
    this.rvfcIntervalCount = 0;
    this.rafIntervalSum = 0;
    this.rafIntervalCount = 0;
    this.uploadFrameId = 0;
    this.swappedFrameId = 0;
    this.rafIntervalWindowIndex = 0;
    this.rafIntervalWindowFilled = 0;

    // Start rVFC loop for frame acquisition
    this.scheduleRvfc();

    // Start rAF loop for drawing
    this.scheduleRaf();
  }

  /**
   * Stop the frame pacing loop.
   */
  stop(): void {
    this.running = false;

    if (this.rvfcHandle && typeof this.video.cancelVideoFrameCallback === 'function') {
      this.video.cancelVideoFrameCallback(this.rvfcHandle);
      this.rvfcHandle = 0;
    }

    if (this.rafHandle) {
      cancelAnimationFrame(this.rafHandle);
      this.rafHandle = 0;
    }
  }

  /**
   * Clean up all WebGL resources.
   */
  destroy(): void {
    this.stop();

    const gl = this.gl;
    if (!gl) return;

    if (this.front?.texture) gl.deleteTexture(this.front.texture);
    if (this.pending?.texture) gl.deleteTexture(this.pending.texture);
    if (this.positionBuffer) gl.deleteBuffer(this.positionBuffer);
    if (this.texCoordBuffer) gl.deleteBuffer(this.texCoordBuffer);
    if (this.program) gl.deleteProgram(this.program);

    this.front = null;
    this.pending = null;
    this.positionBuffer = null;
    this.texCoordBuffer = null;
    this.program = null;
    this.gl = null;
  }

  /**
   * Get current metrics snapshot.
   */
  getMetrics(): Readonly<FramePacerMetrics> {
    return this.metrics;
  }

  /**
   * Update canvas size to match video dimensions.
   * Call when video dimensions change.
   */
  updateCanvasSize(): void {
    const width = this.video.videoWidth;
    const height = this.video.videoHeight;

    if (width > 0 && height > 0) {
      if (this.canvas.width !== width || this.canvas.height !== height) {
        this.canvas.width = width;
        this.canvas.height = height;
        this.gl?.viewport(0, 0, width, height);
      }
    }
  }

  // ─────────────────────────────────────────────────────────────────
  // Percentile calculation (allocation-free using pre-allocated array)
  // ─────────────────────────────────────────────────────────────────

  private computePercentiles(): void {
    const count = this.rafIntervalWindowFilled;
    if (count < 10) return;

    // Copy to temp array for sorting (reuse the same memory)
    const sorted = new Float32Array(count);
    for (let i = 0; i < count; i++) {
      sorted[i] = this.rafIntervalWindow[i];
    }
    sorted.sort();

    const p95Index = Math.floor(count * 0.95);
    const p99Index = Math.floor(count * 0.99);

    this.metrics.p95RafIntervalMs = sorted[Math.min(p95Index, count - 1)];
    this.metrics.p99RafIntervalMs = sorted[Math.min(p99Index, count - 1)];
  }

  // ─────────────────────────────────────────────────────────────────
  // rVFC loop: upload new frames when they become available
  // ─────────────────────────────────────────────────────────────────

  private scheduleRvfc(): void {
    if (!this.running) return;
    if (typeof this.video.requestVideoFrameCallback !== 'function') {
      // Fallback: poll on rAF if rVFC unavailable
      return;
    }
    this.rvfcHandle = this.video.requestVideoFrameCallback(this.onRvfc);
  }

  private onRvfc = (now: DOMHighResTimeStamp, _metadata: VideoFrameCallbackMetadata): void => {
    if (!this.running || !this.gl || !this.pending) {
      this.scheduleRvfc();
      return;
    }

    // Track interval (only if diagnostics enabled)
    if (this._diagnosticsEnabled) {
      if (this.lastRvfcTime > 0) {
        const interval = now - this.lastRvfcTime;
        this.metrics.lastRvfcIntervalMs = interval;
        this.rvfcIntervalSum += interval;
        this.rvfcIntervalCount++;
        this.metrics.avgRvfcIntervalMs = this.rvfcIntervalSum / this.rvfcIntervalCount;
      }
      this.lastRvfcTime = now;
    }

    // Check if video has valid dimensions
    const vw = this.video.videoWidth;
    const vh = this.video.videoHeight;
    if (vw <= 0 || vh <= 0) {
      this.scheduleRvfc();
      return;
    }

    // Ensure canvas matches video size
    this.updateCanvasSize();

    // If pending already has a frame that hasn't been swapped, we're overwriting it
    // This is the "latest-frame-wins" behavior
    if (this._diagnosticsEnabled && this.pending.valid && this.pending.frameId > this.swappedFrameId) {
      this.metrics.framesSkipped++;
    }

    // Upload video to pending texture
    const gl = this.gl;
    gl.bindTexture(gl.TEXTURE_2D, this.pending.texture);

    // Update texture dimensions if needed
    if (this.pending.width !== vw || this.pending.height !== vh) {
      this.pending.width = vw;
      this.pending.height = vh;
    }

    // texImage2D with video element - this is the upload
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, this.video);

    this.uploadFrameId++;
    this.pending.frameId = this.uploadFrameId;
    this.pending.valid = true;
    if (this._diagnosticsEnabled) {
      this.metrics.framesUploaded++;
    }

    // Schedule next rVFC
    this.scheduleRvfc();
  };

  // ─────────────────────────────────────────────────────────────────
  // rAF loop: ALWAYS draw, only control swap
  // ─────────────────────────────────────────────────────────────────

  private scheduleRaf(): void {
    if (!this.running) return;
    this.rafHandle = requestAnimationFrame(this.onRaf);
  }

  private onRaf = (now: DOMHighResTimeStamp): void => {
    if (!this.running) return;

    // Track interval and detect hitches (only if diagnostics enabled)
    if (this._diagnosticsEnabled) {
      if (this.lastRafTime > 0) {
        const interval = now - this.lastRafTime;
        this.metrics.lastRafIntervalMs = interval;
        this.rafIntervalSum += interval;
        this.rafIntervalCount++;
        this.metrics.avgRafIntervalMs = this.rafIntervalSum / this.rafIntervalCount;

        // Hitch detection
        if (interval > HARD_HITCH_THRESHOLD_MS) {
          this.metrics.hardHitchCount++;
          this.metrics.softHitchCount++; // Hard hitches are also soft hitches
        } else if (interval > SOFT_HITCH_THRESHOLD_MS) {
          this.metrics.softHitchCount++;
        }

        // Add to percentile window (circular buffer)
        this.rafIntervalWindow[this.rafIntervalWindowIndex] = interval;
        this.rafIntervalWindowIndex = (this.rafIntervalWindowIndex + 1) % PERCENTILE_WINDOW_SIZE;
        if (this.rafIntervalWindowFilled < PERCENTILE_WINDOW_SIZE) {
          this.rafIntervalWindowFilled++;
        }
      }
      this.lastRafTime = now;
    }

    // Step 1: If pending exists, swap it to front (exactly once)
    if (this.pending?.valid && this.pending.frameId > this.swappedFrameId) {
      const temp = this.front;
      this.front = this.pending;
      this.pending = temp;
      if (this.pending) {
        this.pending.valid = false;
      }
      this.swappedFrameId = this.front!.frameId;
      if (this._diagnosticsEnabled) {
        this.metrics.framesSwapped++;
      }
    }

    // Step 2: ALWAYS draw front buffer (this is "swap pacing")
    if (this.front?.valid) {
      this.drawFrame();
    }

    // Emit metrics periodically (only if diagnostics enabled)
    if (this._diagnosticsEnabled) {
      this.metrics.framesDrawn++;
      if (this.metrics.framesDrawn % 60 === 0) {
        this.computePercentiles();
        this.callbacks.onMetrics?.(this.metrics);
      }
    }

    // Schedule next rAF
    this.scheduleRaf();
  };

  private drawFrame(): void {
    const gl = this.gl;
    const front = this.front;
    if (!gl || !front) return;

    // Bind the front texture and draw
    // (GL state is already set up in init(), we just need to bind texture)
    gl.bindTexture(gl.TEXTURE_2D, front.texture);
    gl.drawArrays(gl.TRIANGLES, 0, 6);
  }
}
