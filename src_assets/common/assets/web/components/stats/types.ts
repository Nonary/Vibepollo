export interface PerformancePoint {
  timestamp: number;
  latencyMs: number | null;
  throughputMbps: number | null;
  qualityEvents: number;
  videoDropped?: number;
  audioDropped?: number;
  fps: number | null;
  /** Identifies a source stream in a merged reconnect history. */
  segment?: string;
}

export interface ChartValuePoint {
  timestamp: number;
  value: number | null;
  segment?: string;
}
