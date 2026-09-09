export interface CrashBundlePart {
  index: number;
  filename?: string;
  estimatedSizeBytes?: number;
}

export interface CrashBundleManifest {
  parts: CrashBundlePart[];
}

/**
 * Keep the manifest contract in one place so a malformed response cannot turn
 * into a partial export that is reported as successful.
 */
export function parseCrashBundleManifest(payload: unknown): CrashBundleManifest | null {
  if (!payload || typeof payload !== 'object' || Array.isArray(payload)) return null;
  const rawParts = (payload as { parts?: unknown }).parts;
  if (!Array.isArray(rawParts) || !rawParts.length) return null;

  const seen = new Set<number>();
  const parts: CrashBundlePart[] = [];
  for (const raw of rawParts) {
    if (!raw || typeof raw !== 'object' || Array.isArray(raw)) return null;
    const index = Number((raw as { index?: unknown }).index);
    if (!Number.isInteger(index) || index < 1 || seen.has(index)) return null;
    seen.add(index);
    const filename = (raw as { filename?: unknown }).filename;
    const estimated = Number((raw as { estimated_size_bytes?: unknown }).estimated_size_bytes);
    parts.push({
      index,
      ...(typeof filename === 'string' && filename.trim() ? { filename: filename.trim() } : {}),
      ...(Number.isFinite(estimated) && estimated >= 0 ? { estimatedSizeBytes: estimated } : {}),
    });
  }

  parts.sort((left, right) => left.index - right.index);
  return { parts };
}

export function parseContentDispositionFilename(header: string | null): string | null {
  if (!header) return null;
  const filenameStar = /filename\*=UTF-8''([^;]+)/i.exec(header);
  if (filenameStar?.[1]) {
    try {
      return decodeURIComponent(filenameStar[1]);
    } catch {
      return filenameStar[1];
    }
  }
  const filename = /filename="?([^";]+)"?/i.exec(header)?.[1];
  return filename || null;
}

export function crashBundlePartPath(index: number): string {
  return `/api/logs/export_crash?part=${encodeURIComponent(String(index))}`;
}
