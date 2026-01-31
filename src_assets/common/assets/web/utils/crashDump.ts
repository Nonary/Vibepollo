export type CrashDumpStatus = {
  available?: boolean;
  filename?: string;
  path?: string;
  process?: string;
  size_bytes?: number;
  captured_at?: string;
  age_seconds?: number;
  age_hours?: number;
  dismissed?: boolean;
  dismissed_at?: string;
};

export const MIN_SUNSHINE_CRASH_DUMP_SIZE_BYTES = 10 * 1024 * 1024;

function isSunshineDump(status?: CrashDumpStatus | null): boolean {
  if (!status) return false;
  const proc = status.process?.toLowerCase();
  if (proc) return proc === 'sunshine.exe';
  const name = status.filename?.toLowerCase() || '';
  return name.startsWith('sunshine.exe.');
}

export function isCrashDumpEligible(status?: CrashDumpStatus | null): boolean {
  if (!status || status.available !== true) {
    return false;
  }
  if (isSunshineDump(status)) {
    const size = status.size_bytes ?? 0;
    return size >= MIN_SUNSHINE_CRASH_DUMP_SIZE_BYTES;
  }
  return true;
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
