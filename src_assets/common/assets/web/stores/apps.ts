import { defineStore } from 'pinia';
import { ref, Ref } from 'vue';
import { http } from '@/http';

export interface PrepCmd {
  do?: string;
  undo?: string;
  elevated?: boolean;
}

export interface App {
  name?: string;
  output?: string;
  cmd?: string | string[];
  uuid?: string;
  'working-dir'?: string;
  'image-path'?: string;
  'exclude-global-prep-cmd'?: boolean;
  'exclude-global-state-cmd'?: boolean;
  'config-overrides'?: Record<string, unknown>;
  'exclude-global-state-cmd'?: boolean;
  'config-overrides'?: Record<string, unknown>;
  elevated?: boolean;
  'auto-detach'?: boolean;
  'wait-all'?: boolean;
  'terminate-on-pause'?: boolean;
  'virtual-display'?: boolean;
  'use-app-identity'?: boolean;
  'per-client-app-identity'?: boolean;
  'allow-client-commands'?: boolean;
  'frame-gen-limiter-fix'?: boolean;
  'gen1-framegen-fix'?: boolean;
  'gen2-framegen-fix'?: boolean;
  'exit-timeout'?: number;
  'prep-cmd'?: PrepCmd[];
  'state-cmd'?: PrepCmd[];
  detached?: string[];
  'scale-factor'?: number;
  gamepad?: string;
  'lossless-scaling-framegen'?: boolean;
  'lossless-scaling-target-fps'?: number;
  'lossless-scaling-rtss-limit'?: number;
  'lossless-scaling-profile'?: string;
  'lossless-scaling-recommended'?: Record<string, unknown>;
  'lossless-scaling-custom'?: Record<string, unknown>;
  'lossless-scaling-launch-delay'?: number;
  // Fallback for any other server fields we don't model yet
  [key: string]: any;
}

interface AppsResponse {
  apps?: App[];
  current_app?: string | null;
  host_uuid?: string;
  host_name?: string;
}

// Centralized store for applications list
export const useAppsStore = defineStore('apps', () => {
  const apps: Ref<App[]> = ref([]);
  const currentAppUuid: Ref<string | null> = ref(null);

  function setApps(list: App[]): void {
    apps.value = Array.isArray(list) ? list : [];
  }

  function setCurrentApp(uuid: unknown): void {
    if (typeof uuid === 'string' && uuid.length > 0) {
      currentAppUuid.value = uuid;
      return;
    }
    currentAppUuid.value = null;
  }

  // Load apps from server. If force is false and apps already present, returns cached list.
  async function loadApps(force = false): Promise<App[]> {
    if (apps.value && apps.value.length > 0 && !force) return apps.value;
    try {
      const r = await http.get<AppsResponse>('./api/apps');
      if (r.status !== 200) {
        setApps([]);
        setCurrentApp(null);
        return apps.value;
      }
      setApps((r.data && r.data.apps) || []);
      setCurrentApp(r.data?.current_app ?? null);
    } catch (e) {
      setApps([]);
      setCurrentApp(null);
    }
    return apps.value;
  }

  async function reorderApps(order: string[]): Promise<{ ok: boolean; error?: string }> {
    try {
      const response = await http.post<{ status?: boolean; error?: string }>(
        './api/apps/reorder',
        { order },
        { validateStatus: () => true },
      );

      if (response.status !== 200) {
        const reason = typeof response.data?.error === 'string' ? response.data.error : undefined;
        return { ok: false, error: reason || `Request failed (${response.status})` };
      }

      if (!response.data?.status) {
        const reason = typeof response.data?.error === 'string' ? response.data.error : undefined;
        return { ok: false, error: reason || 'Server rejected reorder request' };
      }

      await loadApps(true);
      return { ok: true };
    } catch (err) {
      const reason = err instanceof Error ? err.message : undefined;
      return { ok: false, error: reason || 'Failed to reorder applications' };
    }
  }

  async function launchApp(
    uuid: string,
  ): Promise<{ ok: boolean; error?: string; canceled?: boolean }> {
    if (!uuid) {
      return { ok: false, error: 'missing uuid' };
    }
    try {
      const response = await http.post<{ status?: boolean; error?: string }>(
        './api/apps/launch',
        { uuid },
        { validateStatus: () => true },
      );

      if (response.status === 200 && response.data?.status) {
        setCurrentApp(uuid);
        return { ok: true };
      }

      const reason = typeof response.data?.error === 'string' ? response.data.error : undefined;
      return {
        ok: false,
        error: reason || `Request failed (${response.status})`,
      };
    } catch (err) {
      const code = (err as { code?: string } | null)?.code;
      if (code === 'ERR_CANCELED') {
        return { ok: false, canceled: true };
      }
      const reason = err instanceof Error ? err.message : undefined;
      return { ok: false, error: reason || 'Failed to launch application' };
    }
  }

  async function closeActiveApp(): Promise<{ ok: boolean; error?: string }> {
    try {
      const response = await http.post<{ status?: boolean; error?: string }>(
        './api/apps/close',
        {},
        { validateStatus: () => true },
      );

      if (response.status === 200 && response.data?.status) {
        setCurrentApp(null);
        await loadApps(true);
        return { ok: true };
      }

      const reason = typeof response.data?.error === 'string' ? response.data.error : undefined;
      return { ok: false, error: reason || `Request failed (${response.status})` };
    } catch (err) {
      const reason = err instanceof Error ? err.message : undefined;
      return { ok: false, error: reason || 'Failed to close application' };
    }
  }

  return {
    apps,
    setApps,
    loadApps,
    reorderApps,
    launchApp,
    closeActiveApp,
    currentAppUuid,
  };
});
