export interface ChartTimeRange {
  start: number;
  end: number;
}

export function clampChartRange(full: ChartTimeRange, range: ChartTimeRange): ChartTimeRange {
  const fullSpan = Math.max(1, full.end - full.start);
  const requestedSpan = Math.max(1, range.end - range.start);
  if (requestedSpan >= fullSpan) return { ...full };
  const start = Math.max(full.start, Math.min(full.end - requestedSpan, range.start));
  return { start, end: start + requestedSpan };
}

export function zoomChartRange(
  full: ChartTimeRange,
  range: ChartTimeRange,
  timestamp: number,
  factor: number,
  maximumZoom = 12,
): ChartTimeRange {
  const fullSpan = Math.max(1, full.end - full.start);
  const span = Math.max(1, range.end - range.start);
  const safeFactor = Number.isFinite(factor) && factor > 0 ? factor : 1;
  const nextSpan = Math.max(fullSpan / Math.max(1, maximumZoom), span / safeFactor);
  if (nextSpan >= fullSpan) return { ...full };
  const anchor = Math.max(range.start, Math.min(range.end, timestamp));
  const ratio = (anchor - range.start) / span;
  return clampChartRange(full, {
    start: anchor - ratio * nextSpan,
    end: anchor + (1 - ratio) * nextSpan,
  });
}

export function panChartRange(
  full: ChartTimeRange,
  range: ChartTimeRange,
  delta: number,
): ChartTimeRange {
  const shift = Number.isFinite(delta) ? delta : 0;
  return clampChartRange(full, { start: range.start + shift, end: range.end + shift });
}
