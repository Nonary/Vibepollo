import type { HostSeriesField, SessionChartPoint } from './useSessionChartHistory';

type Translate = (key: string) => string;
type Protocol = 'rtsp' | 'webrtc';

interface LineDataset {
  label: string;
  data: number[];
  borderColor: string;
  backgroundColor: string;
  fill: boolean;
}

export interface LineChartData {
  labels: string[];
  datasets: LineDataset[];
}

export declare function buildLatencyChartData(
  labels: string[],
  points: SessionChartPoint[],
  t: Translate,
): LineChartData;
export declare function buildThroughputChartData(
  labels: string[],
  points: SessionChartPoint[],
  t: Translate,
): LineChartData;
export declare function buildQualityChartData(
  labels: string[],
  points: SessionChartPoint[],
  protocol: Protocol,
  t: Translate,
): LineChartData;
export declare function buildFpsChartData(
  labels: string[],
  points: SessionChartPoint[],
  t: Translate,
): LineChartData;
export declare function buildHostComputeChartData(
  labels: string[],
  points: SessionChartPoint[],
  hasHostSeries: (field: HostSeriesField) => boolean,
  t: Translate,
): LineChartData;
export declare function buildHostMemoryChartData(
  labels: string[],
  points: SessionChartPoint[],
  hasHostSeries: (field: HostSeriesField) => boolean,
  t: Translate,
): LineChartData;
export declare function buildHostNetworkChartData(
  labels: string[],
  points: SessionChartPoint[],
  hasHostSeries: (field: HostSeriesField) => boolean,
  t: Translate,
): LineChartData;
