import { describe, expect, it } from 'vitest';
import { computeVideoFrameRenderDelayMs, decideLatencyFenceReset } from '@/utils/webrtc/latency';

describe('WebRTC latency helpers', () => {
  it('computes only positive frame render delay', () => {
    expect(computeVideoFrameRenderDelayMs(120, 100)).toBe(20);
    expect(computeVideoFrameRenderDelayMs(100, 120)).toBe(0);
    expect(computeVideoFrameRenderDelayMs(100, undefined)).toBeUndefined();
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
