export interface CommandRow {
  do: string;
  undo: string;
  elevated?: boolean;
  [key: string]: unknown;
}

export interface ServerCommandRow {
  name: string;
  cmd: string;
  elevated?: boolean;
  [key: string]: unknown;
}

export interface HostHistoryPoint {
  timestamp: number;
  cpu_percent?: number;
  gpu_percent?: number;
  gpu_encoder_percent?: number;
  ram_percent?: number | null;
  vram_percent?: number | null;
  net_rx_bps?: number | null;
  net_tx_bps?: number | null;
}

export interface DisplayFieldVisibility {
  physical: boolean;
  virtual: boolean;
}

function arrayValue(value: unknown): unknown[] {
  if (Array.isArray(value)) return value;
  if (typeof value !== 'string' || !value.trim()) return [];
  try {
    const parsed: unknown = JSON.parse(value);
    return Array.isArray(parsed) ? parsed : [];
  } catch {
    return [];
  }
}

export function normalizeCommandRows(value: unknown, platform: string): CommandRow[] {
  const entries = arrayValue(value);
  const windows = platform.toLocaleLowerCase().includes('windows');
  return entries.map((entry) => {
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

export function normalizeServerCommandRows(value: unknown, platform: string): ServerCommandRow[] {
  const entries = arrayValue(value);
  const windows = platform.toLocaleLowerCase().includes('windows');
  return entries.map((entry) => {
    const source =
      entry && typeof entry === 'object' && !Array.isArray(entry)
        ? (entry as Record<string, unknown>)
        : {};
    const row: ServerCommandRow = {
      ...source,
      name: typeof source.name === 'string' ? source.name : String(source.name ?? ''),
      cmd: typeof source.cmd === 'string' ? source.cmd : String(source.cmd ?? ''),
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

export function serializeServerCommandRows(value: unknown, platform: string): ServerCommandRow[] {
  return normalizeServerCommandRows(value, platform).map((row) => {
    const serialized: ServerCommandRow = {
      ...row,
      name: row.name,
      cmd: row.cmd,
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
  if (value == null || value === '') return null;
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
    (point) => numeric(point.ram_percent),
    (point) => numeric(point.vram_percent),
    (point) => {
      const bytes = numeric(point.net_rx_bps);
      return bytes === null ? null : bytes / 1_000_000;
    },
    (point) => {
      const bytes = numeric(point.net_tx_bps);
      return bytes === null ? null : bytes / 1_000_000;
    },
  ];
  // Keep the first sample in every interior null run. Otherwise a reduced
  // history can join values on either side of an unavailable sample and draw
  // a line the host never reported.
  for (const metric of metrics) {
    for (let index = 1; index < points.length - 1; index += 1) {
      if (metric(points[index]) !== null) continue;
      if (metric(points[index - 1]) !== null) selected.add(index);
    }
  }
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

  if (selected.size >= maximum) {
    return [...selected].sort((left, right) => left - right).map((index) => points[index]);
  }

  const stride = (points.length - 1) / Math.max(1, maximum - 1);
  for (let index = 0; selected.size < maximum && index < maximum; index += 1) {
    selected.add(Math.round(index * stride));
  }
  return [...selected].sort((left, right) => left - right).map((index) => points[index]);
}

export function hostHistoryPeaks(points: HostHistoryPoint[]): {
  cpu: number | null;
  gpu: number | null;
  encoder: number | null;
  networkMbps: number | null;
} {
  const max = (values: Array<number | null>) => {
    const finite = values.filter((value): value is number => value !== null);
    return finite.length ? Math.max(...finite) : null;
  };
  return {
    cpu: max(points.map((point) => boundedPercent(point.cpu_percent))),
    gpu: max(points.map((point) => boundedPercent(point.gpu_percent))),
    encoder: max(points.map((point) => boundedPercent(point.gpu_encoder_percent))),
    networkMbps: max(
      points.map((point) => {
        const bytes = numeric(point.net_tx_bps);
        return bytes === null ? null : bytes / 1_000_000;
      }),
    ),
  };
}

function boundedPercent(value: unknown): number | null {
  const number = numeric(value);
  return number == null || number < 0 ? null : Math.min(100, number);
}
