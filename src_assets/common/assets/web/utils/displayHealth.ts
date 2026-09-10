import { configBoolean } from './settings.ts';

export type WindowsDisplayDriver = 'vibeshine' | 'sudovda';

export type WindowsDisplayDriverState =
  | 'ready'
  | 'failed'
  | 'uninitialized'
  | 'version_incompatible'
  | 'watchdog_failed'
  | 'unknown';

export interface WindowsDisplayDriverMetadata {
  configured?: unknown;
  active?: unknown;
  status?: unknown;
  status_code?: unknown;
}

export interface DisplayMetadataWithDriver {
  platform?: unknown;
  virtual_display_driver?: WindowsDisplayDriverMetadata;
  virtual_display?: { ready?: unknown };
}

export interface WindowsDisplayHealth {
  state: WindowsDisplayDriverState;
  activeDriver?: WindowsDisplayDriver;
  configuredDriver?: WindowsDisplayDriver;
  statusCode?: number;
}

const driverAliases: Record<string, WindowsDisplayDriver> = {
  built_in: 'vibeshine',
  builtin: 'vibeshine',
  sunshine: 'vibeshine',
  sudovda: 'sudovda',
  vibeshine: 'vibeshine',
};

const statusAliases: Record<string, WindowsDisplayDriverState> = {
  ready: 'ready',
  ok: 'ready',
  failed: 'failed',
  error: 'failed',
  uninitialized: 'uninitialized',
  not_initialized: 'uninitialized',
  version_incompatible: 'version_incompatible',
  incompatible: 'version_incompatible',
  watchdog_failed: 'watchdog_failed',
  watchdog: 'watchdog_failed',
  unknown: 'unknown',
};

function normalizeDriver(value: unknown): WindowsDisplayDriver | undefined {
  if (typeof value !== 'string') return undefined;
  return driverAliases[value.trim().toLocaleLowerCase()];
}

function normalizeStatus(value: unknown): WindowsDisplayDriverState {
  if (typeof value === 'number' && Number.isInteger(value)) {
    switch (value) {
      case 0:
        return 'ready';
      case -1:
        return 'failed';
      case -2:
        return 'version_incompatible';
      case -3:
        return 'watchdog_failed';
      case 1:
      default:
        return 'unknown';
    }
  }
  if (typeof value !== 'string') return 'unknown';
  const trimmed = value.trim().toLocaleLowerCase();
  if (/^-?\d+$/.test(trimmed)) return normalizeStatus(Number(trimmed));
  const normalized = trimmed.replaceAll('-', '_').replaceAll(' ', '_');
  return statusAliases[normalized] ?? 'unknown';
}

function numericStatus(value: unknown): number | undefined {
  if (typeof value === 'number' && Number.isInteger(value)) return value;
  if (typeof value === 'string' && /^-?\d+$/.test(value.trim())) return Number(value);
  return undefined;
}

/**
 * Turn the read-only host observation into a UI state.
 *
 * Missing active-driver metadata is intentionally unknown. A config default
 * or a virtual-display readiness flag cannot prove that a driver responded.
 */
export function windowsDisplayHealth(
  metadata: DisplayMetadataWithDriver | null | undefined,
): WindowsDisplayHealth {
  const driver = metadata?.virtual_display_driver;
  const statusCode = numericStatus(driver?.status_code);
  const configuredDriver = normalizeDriver(driver?.configured);
  const activeDriver = normalizeDriver(driver?.active);
  const parsedState = normalizeStatus(driver?.status ?? statusCode);
  return {
    state:
      parsedState === 'unknown' && driver && configuredDriver && !activeDriver
        ? 'uninitialized'
        : parsedState,
    activeDriver,
    configuredDriver,
    statusCode,
  };
}

export function isWindowsHost(metadata: DisplayMetadataWithDriver | null | undefined): boolean {
  return String(metadata?.platform ?? '')
    .toLocaleLowerCase()
    .includes('windows');
}

export interface DummyPlugVsyncState {
  active: boolean;
  forced: boolean;
  locked: boolean;
  effectiveVsyncDisabled: boolean;
}

/**
 * Dummy-plug HDR is a backend-enforced dependency. The editor shows the
 * effective VSYNC state and locks that control only while the workaround is
 * enabled. It does not infer a value from missing or malformed data.
 */
export function dummyPlugVsyncState(
  dummyPlugValue: unknown,
  vsyncValue: unknown,
): DummyPlugVsyncState {
  const active = configBoolean(dummyPlugValue);
  const vsyncDisabled = configBoolean(vsyncValue);
  return {
    active,
    forced: active && !vsyncDisabled,
    locked: active,
    effectiveVsyncDisabled: active || vsyncDisabled,
  };
}

/**
 * Apply the only automatic repair permitted by the dependency: enabling the
 * workaround turns VSYNC-off on. Disabling it preserves the user's current
 * VSYNC choice so an explicit override is never silently erased.
 */
export function applyDummyPlugVsyncChange(
  values: Record<string, unknown>,
  dummyPlugEnabled: unknown,
): Record<string, unknown> {
  const next: Record<string, unknown> = {
    ...values,
    dd_wa_dummy_plug_hdr10: configBoolean(dummyPlugEnabled),
  };
  if (next.dd_wa_dummy_plug_hdr10 === true) next.frame_limiter_disable_vsync = true;
  return next;
}
