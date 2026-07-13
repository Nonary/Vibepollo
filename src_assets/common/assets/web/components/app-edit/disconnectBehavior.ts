import type { AppDisconnectBehavior } from './types';

export type GlobalDisconnectBehavior = Exclude<AppDisconnectBehavior, 'inherit' | 'terminate'>;
export const APP_DISCONNECT_BEHAVIOR_VALUES = [
  'inherit',
  'keep_running',
  'suspend',
  'terminate',
] as const satisfies readonly AppDisconnectBehavior[];

function parseBoolean(value: unknown, fallback: boolean): boolean {
  if (typeof value === 'boolean') return value;
  if (typeof value === 'number') return value !== 0;
  const normalized = String(value ?? '')
    .toLowerCase()
    .trim();
  if (['true', '1', 'enabled', 'enable', 'yes', 'on'].includes(normalized)) return true;
  if (['false', '0', 'disabled', 'disable', 'no', 'off'].includes(normalized)) return false;
  return fallback;
}

export function parseAppDisconnectBehavior(
  value: unknown,
  legacyTerminateOnPause: unknown,
): AppDisconnectBehavior {
  const normalized = String(value ?? '')
    .toLowerCase()
    .trim();
  if (APP_DISCONNECT_BEHAVIOR_VALUES.includes(normalized as AppDisconnectBehavior)) {
    return normalized as AppDisconnectBehavior;
  }
  return parseBoolean(legacyTerminateOnPause, false) ? 'terminate' : 'inherit';
}

export function normalizeGlobalDisconnectBehavior(value: unknown): GlobalDisconnectBehavior {
  return String(value ?? '').toLowerCase().trim() === 'suspend' ? 'suspend' : 'keep_running';
}

export function effectiveAppDisconnectBehavior(
  appValue: AppDisconnectBehavior,
  globalValue: unknown,
): Exclude<AppDisconnectBehavior, 'inherit'> {
  return appValue === 'inherit' ? normalizeGlobalDisconnectBehavior(globalValue) : appValue;
}

export function disconnectBehaviorPayload(
  value: AppDisconnectBehavior,
): Record<string, AppDisconnectBehavior> {
  return value === 'inherit' ? {} : { 'disconnect-behavior': value };
}

export function shouldShowSuspendWarning(
  appValue: AppDisconnectBehavior,
  globalValue: unknown,
): boolean {
  return effectiveAppDisconnectBehavior(appValue, globalValue) === 'suspend';
}
