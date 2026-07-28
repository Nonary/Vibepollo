import { describe, expect, it } from 'vitest';
import {
  computeVideoFrameCaptureToDisplayAgeMs,
  computeVideoFrameRenderDelayMs,
  decideLatencyFenceReset,
  selectPreferredVideoLatencySignal,
} from '@/utils/webrtc/latency';

describe('WebRTC latency helpers', () => {
  it('computes only positive frame render delay', () => {
    expect(computeVideoFrameRenderDelayMs(120, 100)).toBe(20);
    expect(computeVideoFrameRenderDelayMs(100, 120)).toBe(0);
    expect(computeVideoFrameRenderDelayMs(100, undefined)).toBeUndefined();
  });

  it('computes capture-to-display age only for valid rVFC timestamps', () => {
    expect(computeVideoFrameCaptureToDisplayAgeMs(100, 125)).toBe(25);
    expect(computeVideoFrameCaptureToDisplayAgeMs(0, 20)).toBe(20);
    expect(computeVideoFrameCaptureToDisplayAgeMs(undefined, 125)).toBeUndefined();
    expect(computeVideoFrameCaptureToDisplayAgeMs(Number.NaN, 125)).toBeUndefined();
    expect(computeVideoFrameCaptureToDisplayAgeMs(125, 100)).toBeUndefined();
    expect(computeVideoFrameCaptureToDisplayAgeMs(0, 60_001)).toBeUndefined();
  });

  it('prefers measured frame freshness to the jitter-buffer proxy', () => {
    expect(selectPreferredVideoLatencySignal(18, 90)).toEqual({
      source: 'capture-to-display',
      valueMs: 18,
    });
    expect(selectPreferredVideoLatencySignal(undefined, 90)).toEqual({
      source: 'jitter-buffer',
      valueMs: 90,
    });
    expect(selectPreferredVideoLatencySignal(-1, undefined)).toBeUndefined();
  });

  it('waits for sustained lag before resetting', () => {
    const first = decideLatencyFenceReset({
      valueMs: 150,
      thresholdMs: 100,
      sustainMs: 500,
      cooldownMs: 2000,
      overloadedSinceMs: null,
      lastResetAtMs: null,
      nowMs: 1000,
    });
    expect(first.shouldReset).toBe(false);
    expect(first.overloadedSinceMs).toBe(1000);

    const second = decideLatencyFenceReset({
      valueMs: 150,
      thresholdMs: 100,
      sustainMs: 500,
      cooldownMs: 2000,
      overloadedSinceMs: first.overloadedSinceMs,
      lastResetAtMs: first.lastResetAtMs,
      nowMs: 1499,
    });
    expect(second.shouldReset).toBe(false);
    expect(second.overloadedSinceMs).toBe(1000);

    const third = decideLatencyFenceReset({
      valueMs: 150,
      thresholdMs: 100,
      sustainMs: 500,
      cooldownMs: 2000,
      overloadedSinceMs: second.overloadedSinceMs,
      lastResetAtMs: second.lastResetAtMs,
      nowMs: 1500,
    });
    expect(third.shouldReset).toBe(true);
    expect(third.overloadedSinceMs).toBeNull();
    expect(third.lastResetAtMs).toBe(1500);
  });

  it('respects reset cooldown while keeping the fence armed', () => {
    const decision = decideLatencyFenceReset({
      valueMs: 180,
      thresholdMs: 100,
      sustainMs: 500,
      cooldownMs: 2000,
      overloadedSinceMs: 2000,
      lastResetAtMs: 2500,
      nowMs: 3000,
    });

    expect(decision.shouldReset).toBe(false);
    expect(decision.overloadedSinceMs).toBe(2000);
    expect(decision.lastResetAtMs).toBe(2500);
  });
});
