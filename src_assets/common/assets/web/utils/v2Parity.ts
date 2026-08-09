export interface CommandRow {
  do: string;
  undo: string;
  elevated?: boolean;
  [key: string]: unknown;
}

export interface HostHistoryPoint {
  timestamp: number;
  cpu_percent?: number;
  gpu_percent?: number;
  gpu_encoder_percent?: number;
  net_tx_bps?: number | null;
}

export interface DisplayFieldVisibility {
  physical: boolean;
  virtual: boolean;
}

export function normalizeCommandRows(value: unknown, platform: string): CommandRow[] {
  if (!Array.isArray(value)) return [];
  const windows = platform.toLocaleLowerCase().includes('windows');
  return value.map((entry) => {
    const source =
      entry && typeof entry === 'object' && !Array.isArray(entry)
        ? (entry as Record<string, unknown>)
        : {};
    const row: CommandRow = {
      ...source,
      do: typeof source.do === 'string' ? source.do : String(source.do ?? ''),
      undo: typeof source.undo === 'string' ? source.undo : String(source.undo ?? ''),
    };
    if (windows) row.elevated = source.elevated === true;
    else delete row.elevated;
    return row;
  });
}

export function serializeCommandRows(value: unknown, platform: string): CommandRow[] {
  return normalizeCommandRows(value, platform).map((row) => {
    const serialized: CommandRow = {
      ...row,
      do: row.do,
      undo: row.undo,
    };
    if (platform.toLocaleLowerCase().includes('windows'))
      serialized.elevated = row.elevated === true;
    else delete serialized.elevated;
    return serialized;
  });
}

export function displayFieldVisibility(mode: unknown): DisplayFieldVisibility {
  const physical =
    String(mode ?? '')
      .trim()
      .toLocaleLowerCase() === 'disabled';
  return { physical, virtual: !physical };
}

export function preserveHiddenDisplayValues(
  previous: Record<string, unknown>,
  patch: Record<string, unknown>,
): Record<string, unknown> {
  return { ...previous, ...patch };
}

function numeric(value: unknown): number | null {
  const number = Number(value);
  return Number.isFinite(number) ? number : null;
}

export function downsampleHostHistory(
  points: HostHistoryPoint[],
  maximum = 120,
): HostHistoryPoint[] {
  if (points.length <= maximum) return [...points];
  const selected = new Set<number>();
  const metrics: Array<(point: HostHistoryPoint) => number | null> = [
    (point) => numeric(point.cpu_percent),
    (point) => numeric(point.gpu_percent),
    (point) => numeric(point.gpu_encoder_percent),
    (point) => {
      const bytes = numeric(point.net_tx_bps);
      return bytes === null ? null : bytes / 1_000_000;
    },
  ];
  for (const metric of metrics) {
    let peakIndex = -1;
    let peakValue = -Infinity;
    points.forEach((point, index) => {
      const value = metric(point);
      if (value !== null && value > peakValue) {
        peakValue = value;
        peakIndex = index;
      }
    });
    if (peakIndex >= 0) selected.add(peakIndex);
  }

  if (selected.size < maximum) selected.add(0);
  if (selected.size < maximum) selected.add(points.length - 1);

  const stride = (points.length - 1) / Math.max(1, maximum - 1);
  for (let index = 0; selected.size < maximum && index < maximum; index += 1) {
    selected.add(Math.round(index * stride));
  }
  return [...selected]
    .sort((left, right) => left - right)
    .slice(0, maximum)
    .map((index) => points[index]);
}

export function hostHistoryPeaks(points: HostHistoryPoint[]): {
  cpu: number;
  gpu: number;
  encoder: number;
  networkMbps: number;
} {
  const max = (values: Array<number | null>) =>
    Math.max(0, ...values.filter((value): value is number => value !== null));
  return {
    cpu: max(points.map((point) => numeric(point.cpu_percent))),
    gpu: max(points.map((point) => numeric(point.gpu_percent))),
    encoder: max(points.map((point) => numeric(point.gpu_encoder_percent))),
    networkMbps: max(
      points.map((point) => {
        const bytes = numeric(point.net_tx_bps);
        return bytes === null ? null : bytes / 1_000_000;
      }),
    ),
  };
}
