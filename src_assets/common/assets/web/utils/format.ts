function unitFormatter(
  value: number,
  unit: string,
  locale?: string,
  maximumFractionDigits = 0,
): string {
  return new Intl.NumberFormat(locale || undefined, {
    maximumFractionDigits,
    style: 'unit',
    unit,
    unitDisplay: 'narrow',
  }).format(value);
}

export function formatDuration(totalSeconds: number, locale?: string): string {
  const seconds = Math.max(0, Math.floor(totalSeconds || 0));
  const hours = Math.floor(seconds / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  const remainder = seconds % 60;
  if (hours) {
    return `${unitFormatter(hours, 'hour', locale)} ${unitFormatter(minutes, 'minute', locale)}`;
  }
  if (minutes) {
    return `${unitFormatter(minutes, 'minute', locale)} ${unitFormatter(remainder, 'second', locale)}`;
  }
  return unitFormatter(remainder, 'second', locale);
}

export function formatBytes(bytes: number, locale?: string): string {
  const units: string[] = [
    'byte',
    'kilobyte',
    'megabyte',
    'gigabyte',
    'terabyte',
  ];
  if (!Number.isFinite(bytes) || bytes <= 0) return unitFormatter(0, units[0], locale);
  const exponent = Math.min(Math.floor(Math.log(bytes) / Math.log(1024)), units.length - 1);
  const value = bytes / 1024 ** exponent;
  return unitFormatter(value, units[exponent], locale, value >= 10 || exponent === 0 ? 0 : 1);
}

export function formatBitrate(kbps: number, locale?: string): string {
  if (!Number.isFinite(kbps) || kbps <= 0) {
    return unitFormatter(0, 'kilobit-per-second', locale);
  }
  return kbps >= 1000
    ? unitFormatter(kbps / 1000, 'megabit-per-second', locale, 1)
    : unitFormatter(Math.round(kbps), 'kilobit-per-second', locale);
}

export function formatRelativeTime(
  timestamp: number | string | Date,
  locale?: string,
  invalidFallback = '',
): string {
  const date = timestamp instanceof Date ? timestamp : new Date(timestamp);
  const delta = date.getTime() - Date.now();
  if (!Number.isFinite(delta)) return invalidFallback;

  const formatter = new Intl.RelativeTimeFormat(locale || undefined, { numeric: 'auto' });
  const ranges: Array<[Intl.RelativeTimeFormatUnit, number]> = [
    ['year', 365 * 24 * 60 * 60 * 1000],
    ['month', 30 * 24 * 60 * 60 * 1000],
    ['day', 24 * 60 * 60 * 1000],
    ['hour', 60 * 60 * 1000],
    ['minute', 60 * 1000],
    ['second', 1000],
  ];
  for (const [unit, size] of ranges) {
    if (Math.abs(delta) >= size || unit === 'second') {
      return formatter.format(Math.round(delta / size), unit);
    }
  }
  return formatter.format(0, 'second');
}

export function asArray<T>(payload: unknown, key: string): T[] {
  if (Array.isArray(payload)) return payload as T[];
  if (payload && typeof payload === 'object') {
    const value = (payload as Record<string, unknown>)[key];
    if (Array.isArray(value)) return value as T[];
  }
  return [];
}
