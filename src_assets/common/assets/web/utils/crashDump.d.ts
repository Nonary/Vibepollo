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

export declare const MIN_SUNSHINE_CRASH_DUMP_SIZE_BYTES: number;
export declare function isCrashDumpEligible(status?: CrashDumpStatus | null): boolean;
export declare function sanitizeCrashDumpStatus(
  status?: CrashDumpStatus | null,
): CrashDumpStatus | null;
