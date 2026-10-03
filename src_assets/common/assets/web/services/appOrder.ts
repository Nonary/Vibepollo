import type { AppRecord } from '@/services/apps';

export type AppOrderGroup = 'custom' | 'steam' | 'playnite' | 'lutris';
export type AppOrderProvider = Exclude<AppOrderGroup, 'custom'>;
export type AppOrderSort = 'name' | 'recent' | 'playtime';

export const APP_ORDER_GROUPS: AppOrderGroup[] = ['custom', 'steam', 'playnite', 'lutris'];
export const APP_ORDER_SORTS: AppOrderSort[] = ['name', 'recent', 'playtime'];

function text(value: unknown): string {
  return value === undefined || value === null ? '' : String(value).trim().toLowerCase();
}

/** Parses app_order_<provider>; unknown values fall back to name like the host. */
export function parseAppOrderSort(value: unknown): AppOrderSort {
  const sort = text(value) as AppOrderSort;
  return APP_ORDER_SORTS.includes(sort) ? sort : 'name';
}

/** Parses app_order_groups the same way as proc::app_order::parse. */
export function parseAppOrderGroups(value: unknown): AppOrderGroup[] {
  const groups: AppOrderGroup[] = [];
  for (const token of text(value).split(',')) {
    const group = token.trim() as AppOrderGroup;
    if (APP_ORDER_GROUPS.includes(group) && !groups.includes(group)) groups.push(group);
  }
  return groups;
}

function hasId(app: AppRecord, key: string): boolean {
  const value = app[key];
  return typeof value === 'string' && Boolean(value.trim());
}

/** Mirrors proc::app_order::group_of on the host. */
export function appOrderGroup(app: AppRecord): AppOrderGroup {
  if (hasId(app, 'playnite-id')) return 'playnite';
  if (hasId(app, 'steam-id')) return 'steam';
  if (hasId(app, 'lutris-id')) return 'lutris';
  return 'custom';
}
