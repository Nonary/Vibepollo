export interface LatencyFenceInput {
  valueMs: number | null | undefined;
  thresholdMs: number;
  sustainMs: number;
  cooldownMs: number;
  overloadedSinceMs: number | null;
  lastResetAtMs: number | null;
  nowMs: number;
}

export interface LatencyFenceDecision {
  overloadedSinceMs: number | null;
  lastResetAtMs: number | null;
  shouldReset: boolean;
}

export type VideoLatencyControlSource = 'capture-to-display' | 'jitter-buffer';

export interface VideoLatencyControlSignal {
  source: VideoLatencyControlSource;
  valueMs: number;
}

// A larger value almost certainly means that the two timestamps use different
// clock origins. A real queue should be recovered well before this limit.
const MAX_CAPTURE_TO_DISPLAY_AGE_MS = 60_000;

function isFiniteNumber(value: unknown): value is number {
  return typeof value === 'number' && Number.isFinite(value);
}

export function computeVideoFrameRenderDelayMs(
  nowMs: number,
  expectedDisplayTimeMs?: number,
): number | undefined {
  if (!isFiniteNumber(nowMs) || !isFiniteNumber(expectedDisplayTimeMs)) return undefined;
  return Math.max(0, nowMs - expectedDisplayTimeMs);
}

/**
 * Computes the age of a WebRTC frame at its expected presentation time.
 *
 * Both values are rVFC DOMHighResTimeStamps. `captureTime` is optional, and
 * implementations without a synchronized capture clock may omit it or report
 * unusable values, so callers must retain a fallback signal.
 */
export function computeVideoFrameCaptureToDisplayAgeMs(
  captureTimeMs: unknown,
  expectedDisplayTimeMs: unknown,
): number | undefined {
  if (!isFiniteNumber(captureTimeMs) || !isFiniteNumber(expectedDisplayTimeMs)) return undefined;
  if (captureTimeMs < 0 || expectedDisplayTimeMs < 0) return undefined;

  const ageMs = expectedDisplayTimeMs - captureTimeMs;
  if (ageMs < 0 || ageMs > MAX_CAPTURE_TO_DISPLAY_AGE_MS) return undefined;
  return ageMs;
}

/**
 * Prefer measured displayed-frame freshness over the receiver jitter-buffer
 * proxy whenever rVFC supplies it. The two values share the same "extra
 * latency" semantics at this point: capture age is normalized by the caller's
 * healthy baseline before it reaches this helper.
 */
export function selectPreferredVideoLatencySignal(
  captureToDisplayDriftMs: number | null | undefined,
  jitterBufferDelayMs: number | null | undefined,
): VideoLatencyControlSignal | undefined {
  if (isFiniteNumber(captureToDisplayDriftMs) && captureToDisplayDriftMs >= 0) {
    return { source: 'capture-to-display', valueMs: captureToDisplayDriftMs };
  }
  if (isFiniteNumber(jitterBufferDelayMs) && jitterBufferDelayMs >= 0) {
    return { source: 'jitter-buffer', valueMs: jitterBufferDelayMs };
  }
  return undefined;
}

export function decideLatencyFenceReset(input: LatencyFenceInput): LatencyFenceDecision {
  const { valueMs, thresholdMs, sustainMs, cooldownMs, overloadedSinceMs, lastResetAtMs, nowMs } =
    input;

  if (!isFiniteNumber(valueMs) || valueMs < thresholdMs) {
    return { overloadedSinceMs: null, lastResetAtMs, shouldReset: false };
  }

  const nextOverloadedSinceMs = overloadedSinceMs ?? nowMs;
  if (nowMs - nextOverloadedSinceMs < sustainMs) {
    return {
      overloadedSinceMs: nextOverloadedSinceMs,
      lastResetAtMs,
      shouldReset: false,
    };
  }

  if (lastResetAtMs != null && nowMs - lastResetAtMs < cooldownMs) {
    return {
      overloadedSinceMs: nextOverloadedSinceMs,
      lastResetAtMs,
      shouldReset: false,
    };
  }

  return {
    overloadedSinceMs: null,
    lastResetAtMs: nowMs,
    shouldReset: true,
  };
}
