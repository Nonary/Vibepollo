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
  // Fallback for any other server fields we don't model yet
  [key: string]: any;
}

interface AppsResponse {
  apps?: App[];
}

// Centralized store for applications list
export const useAppsStore = defineStore('apps', () => {
  const apps: Ref<App[]> = ref([]);

  function setApps(list: App[]): void {
    apps.value = Array.isArray(list) ? list : [];
  }

  // Load apps from server. If force is false and apps already present, returns cached list.
  async function loadApps(force = false): Promise<App[]> {
    if (apps.value && apps.value.length > 0 && !force) return apps.value;
    try {
      const r = await http.get<AppsResponse>('./api/apps');
      if (r.status !== 200) {
        setApps([]);
        return apps.value;
      }
      setApps((r.data && r.data.apps) || []);
    } catch (e) {
      setApps([]);
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

  return {
    apps,
    setApps,
    loadApps,
    reorderApps,
  };
});
