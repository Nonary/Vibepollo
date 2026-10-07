type ChartOptionsShape = Record<string, unknown>;

export declare function buildBaseChartOptions(
  eventAnnotations: Record<string, unknown>,
): ChartOptionsShape;
export declare function buildLatencyChartOptions(base: ChartOptionsShape): ChartOptionsShape;
export declare function buildQualityChartOptions(base: ChartOptionsShape): ChartOptionsShape;
export declare function buildFpsChartOptions(
  base: ChartOptionsShape,
  targetFps: number,
): ChartOptionsShape;
export declare function buildHostPercentChartOptions(base: ChartOptionsShape): ChartOptionsShape;
export declare function buildHostNetworkChartOptions(base: ChartOptionsShape): ChartOptionsShape;
export declare function withZoom<T extends { plugins?: Record<string, unknown> }>(options: T): T;
