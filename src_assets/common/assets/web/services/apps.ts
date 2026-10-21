import { apiDelete, apiGet, apiPost } from '@/api/client';
import type { ServerApp } from '@/components/app-edit/types';
import { asArray } from '@/utils/format';

export type AppRecord = ServerApp & Record<string, unknown>;

export interface AppMutationResult extends Record<string, unknown> {
  status?: boolean;
  removed?: number;
}

export type AppServiceErrorCode = 'missing-app-uuid' | 'save-rejected';

export class AppServiceError extends Error {
  readonly code: AppServiceErrorCode;

  constructor(code: AppServiceErrorCode, message: string = code) {
    super(message);
    this.name = 'AppServiceError';
    this.code = code;
  }
}

const transientFields = new Set([
  'id',
  'index',
  'image-version',
  'playnite-icon-version',
  'remote-session',
]);

export async function fetchApps(): Promise<AppRecord[]> {
  const payload = await apiGet<unknown>('/api/apps');
  return asArray<AppRecord>(payload, 'apps').filter((app): app is AppRecord =>
    Boolean(app && typeof app === 'object' && !Array.isArray(app)),
  );
}

export function appUuid(app: AppRecord): string {
  return typeof app.uuid === 'string' ? app.uuid : '';
}

export function appName(app: AppRecord): string {
  return typeof app.name === 'string' ? app.name.trim() : '';
}

export function appCoverUrl(app: AppRecord): string {
  const uuid = appUuid(app);
  if (!uuid) return '';
  const version = app['image-version'];
  const suffix =
    typeof version === 'number' || typeof version === 'string'
      ? `?v=${encodeURIComponent(String(version))}`
      : '';
  return `/api/apps/${encodeURIComponent(uuid)}/cover${suffix}`;
}

export function prepareAppForSave(app: AppRecord): AppRecord {
  const payload = structuredClone(app);
  for (const field of transientFields) delete payload[field];
  payload['prep-cmd'] = Array.isArray(payload['prep-cmd']) ? payload['prep-cmd'] : [];
  payload.detached = Array.isArray(payload.detached) ? payload.detached : [];
  return payload;
}

export async function saveApp(app: AppRecord): Promise<AppMutationResult> {
  const result = await apiPost<AppMutationResult>('/api/apps', prepareAppForSave(app));
  if (result.status === false) {
    throw new AppServiceError('save-rejected', typeof result.error === 'string' ? result.error : undefined);
  }
  return result;
}

export function deleteApp(uuid: string, options: { providerMaintenance?: boolean } = {}): Promise<AppMutationResult> {
  if (!uuid) return Promise.reject(new AppServiceError('missing-app-uuid'));
  return apiDelete<AppMutationResult>(`/api/apps/${encodeURIComponent(uuid)}`,
    options.providerMaintenance ? { 'provider-maintenance': true } : undefined);
}
