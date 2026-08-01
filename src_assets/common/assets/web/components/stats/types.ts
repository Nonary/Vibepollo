export interface PerformancePoint {
  timestamp: number;
  latencyMs: number | null;
  throughputMbps: number;
  qualityEvents: number;
  fps: number;
}
