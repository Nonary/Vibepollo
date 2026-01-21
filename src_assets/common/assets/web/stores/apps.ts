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
  'config-overrides'?: Record<string, unknown>;
  elevated?: boolean;
  'auto-detach'?: boolean;
  'wait-all'?: boolean;
  'frame-gen-limiter-fix'?: boolean;
  'gen1-framegen-fix'?: boolean;
  'gen2-framegen-fix'?: boolean;
  'exit-timeout'?: number;
  'prep-cmd'?: PrepCmd[];
  detached?: string[];
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

  return {
    apps,
    setApps,
    loadApps,
  };
});
