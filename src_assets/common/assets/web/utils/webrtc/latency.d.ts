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

export declare function computeVideoFrameRenderDelayMs(
  nowMs: number,
  expectedDisplayTimeMs?: number,
): number | undefined;
export declare function computeVideoFrameCaptureToDisplayAgeMs(
  captureTimeMs: unknown,
  expectedDisplayTimeMs: unknown,
): number | undefined;
export declare function selectPreferredVideoLatencySignal(
  captureToDisplayDriftMs: number | null | undefined,
  jitterBufferDelayMs: number | null | undefined,
): VideoLatencyControlSignal | undefined;
export declare function decideLatencyFenceReset(input: LatencyFenceInput): LatencyFenceDecision;
