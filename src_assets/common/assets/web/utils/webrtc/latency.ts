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
