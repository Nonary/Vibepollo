export type CrashDumpStatus = {
  available?: boolean;
  filename?: string;
  path?: string;
  size_bytes?: number;
  captured_at?: string;
  age_seconds?: number;
  age_hours?: number;
  dismissed?: boolean;
  dismissed_at?: string;
};

export const MIN_CRASH_DUMP_SIZE_BYTES = 10 * 1024 * 1024;

export function isCrashDumpEligible(status?: CrashDumpStatus | null): boolean {
  if (!status || status.available !== true) {
    return false;
  }
  const size = status.size_bytes ?? 0;
  return size >= MIN_CRASH_DUMP_SIZE_BYTES;
}

export function sanitizeCrashDumpStatus(status?: CrashDumpStatus | null): CrashDumpStatus | null {
  if (!status) {
    return null;
  }
  if (status.available !== true) {
    return status;
  }
  if (!isCrashDumpEligible(status)) {
    return { available: false };
  }
  return status;
}
