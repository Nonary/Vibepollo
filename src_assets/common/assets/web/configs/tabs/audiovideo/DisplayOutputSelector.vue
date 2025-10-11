<script setup lang="ts">
import { computed, onMounted, onBeforeUnmount, ref, watch } from 'vue';
import { $tp } from '@/platform-i18n';
import { useI18n } from 'vue-i18n';
import PlatformLayout from '@/PlatformLayout.vue';
import { NInput, NSelect } from 'naive-ui';
import { useConfigStore } from '@/stores/config';
import { http } from '@/http';

type DisplayDevice = {
  device_id?: string;
  display_name?: string; // e.g. \\ \\.\\DISPLAY1
  friendly_name?: string; // e.g. ROG PG279Q
  is_virtual?: boolean; // True if this is a SudaVDA virtual display
  is_isolated?: boolean; // True if this is intended as an isolated monitor
  // Present when device is currently active; shape mirrors libdisplaydevice types but we only check presence
  info?: unknown;
};

const store = useConfigStore();
const config = store.config;
// Read platform directly from store metadata to avoid timing/race on wrapper
const platform = computed(() => (store.metadata && store.metadata.platform) || '');

const devices = ref<DisplayDevice[]>([]);
const loading = ref(false);
const loadError = ref('');
const { t } = useI18n();

function tFirst(keys: string[], fallback: string): string {
  for (const k of keys) {
    const m = t(k) as unknown as string;
    if (m && m !== k) return m;
  }
  return fallback;
}

const outputNameLabel = computed(() =>
  tFirst(['config.output_name', 'offline.output_name'], 'Display Id'),
);
const outputNameDefaultLabel = computed(() =>
  tFirst(
    ['offline.output_name_default', 'config.output_name_default'],
    'Primary display (default)',
  ),
);
const outputNameDesc = computed(() =>
  $tp('config.output_name_desc', $tp('offline.output_name_desc', '')),
);

async function loadDisplayDevices() {
  loading.value = true;
  loadError.value = '';
  try {
    const res = await http.get<DisplayDevice[]>('/api/display-devices');
    const arr = Array.isArray(res.data) ? res.data : [];
    devices.value = arr;
  } catch (e: any) {
    // Non-fatal: keep manual entry available as fallback
    loadError.value = e?.message || 'Failed to load display devices';
    devices.value = [];
  } finally {
    loading.value = false;
  }
}

onMounted(() => {
  // Proactively load once on mount; backend gracefully handles non-Windows
  if (!loading.value && devices.value.length === 0) void loadDisplayDevices();
});

// If platform metadata arrives after mount, load then
const stopWatch = watch(
  () => platform.value,
  (p) => {
    if (p === 'windows' && devices.value.length === 0 && !loading.value) {
      void loadDisplayDevices();
    }
  },
  { immediate: false },
);

onBeforeUnmount(() => {
  stopWatch();
});

const outputNamePlaceholder = computed(() =>
  platform.value === 'windows' ? '{de9bb7e2-186e-505b-9e93-f48793333810}' : '0',
);

function toOptions() {
  // First option represents default behavior (primary display)
  const opts: Array<{
    label: string;
    value: string;
    displayName?: string;
    id?: string;
    isVirtual?: boolean;
    isIsolated?: boolean;
    hideIdentifier?: boolean;
  }> = [
    {
      label: outputNameDefaultLabel.value,
      value: '',
      displayName: outputNameDefaultLabel.value,
      id: '',
    },
  ];

  for (const d of devices.value) {
    const displayName = d.friendly_name || d.display_name || 'Display';
    const guid = d.device_id || '';
    const dispName = d.display_name || '';
    const hideIdentifier = Boolean(d.is_virtual);
    const composedId = hideIdentifier ? '' : (guid && dispName ? `${guid} - ${dispName}` : guid || dispName);
    const parts: string[] = [displayName];
    if (!hideIdentifier && guid) parts.push(guid);
    if (!hideIdentifier && dispName) parts.push(dispName + (d.info ? ' (active)' : ''));
    const label = parts.join(' - ');
    const value = d.device_id || d.display_name || '';
    if (value)
      opts.push({
        label,
        value,
        displayName,
        id: composedId || undefined,
        isVirtual: d.is_virtual ?? undefined,
        isIsolated: d.is_isolated ?? undefined,
        hideIdentifier: hideIdentifier || undefined,
      });
  }
  return opts;
}

</script>

<template>
  <div class="mb-4">
    <label for="output_name" class="form-label">{{ outputNameLabel }}</label>

    <!-- Windows: dropdown of available displays from API -->
    <PlatformLayout>
      <template #windows>
        <n-select
          id="output_name"
          v-model:value="config.output_name"
          :options="toOptions()"
          :loading="loading"
          @focus="
            () => {
              if (!loading && devices.length === 0) void loadDisplayDevices();
            }
          "
          clearable
          filterable
          :placeholder="outputNameLabel"
        >
          <!-- Render each option with the friendly/display name on top and the id underneath in monospace -->
          <template #option="{ option }">
            <div class="leading-tight">
              <div class="flex items-center gap-2">
                <span>{{ option?.displayName || option?.label }}</span>
                <span
                  v-if="option?.isVirtual"
                  class="text-[10px] px-1.5 py-0.5 rounded bg-purple-500/20 text-purple-400 border border-purple-500/30"
                >
                  Virtual
                </span>
                <span
                  v-if="option?.isIsolated"
                  class="text-[10px] px-1.5 py-0.5 rounded bg-blue-500/20 text-blue-400 border border-blue-500/30"
                >
                  Isolated
                </span>
              </div>
              <div v-if="!option?.hideIdentifier" class="text-[12px] opacity-60 font-mono">
                {{ option?.id || option?.value }}
              </div>
            </div>
          </template>

          <!-- Show the selected value similarly: name then id -->
          <template #value="{ option }">
            <div class="leading-tight">
              <div class="flex items-center gap-2">
                <span>{{ option?.displayName || option?.label }}</span>
                <span
                  v-if="option?.isVirtual"
                  class="text-[10px] px-1.5 py-0.5 rounded bg-purple-500/20 text-purple-400 border border-purple-500/30"
                >
                  Virtual
                </span>
                <span
                  v-if="option?.isIsolated"
                  class="text-[10px] px-1.5 py-0.5 rounded bg-blue-500/20 text-blue-400 border border-blue-500/30"
                >
                  Isolated
                </span>
              </div>
              <div v-if="!option?.hideIdentifier" class="text-[12px] opacity-60 font-mono">
                {{ option?.id || option?.value }}
              </div>
            </div>
          </template>
        </n-select>
      </template>
      <template #linux>
        <n-input
          id="output_name"
          v-model:value="config.output_name"
          type="text"
          :placeholder="outputNamePlaceholder"
        />
      </template>
      <template #macos>
        <n-input
          id="output_name"
          v-model:value="config.output_name"
          type="text"
          :placeholder="outputNamePlaceholder"
        />
      </template>
    </PlatformLayout>
    <div class="text-[11px] opacity-60">
      {{ outputNameDesc }}<br />
      <template v-if="platform === 'windows' && loadError">
        <span class="text-red-500">{{ loadError }}</span
        ><br />
      </template>
      <PlatformLayout>
        <template #windows>
          <pre style="white-space: pre-line">
            <b>&nbsp;&nbsp;{</b>
            <b>&nbsp;&nbsp;&nbsp;&nbsp;"device_id": "{de9bb7e2-186e-505b-9e93-f48793333810}"</b>
            <b>&nbsp;&nbsp;&nbsp;&nbsp;"display_name": "\\\\.\\DISPLAY1"</b>
            <b>&nbsp;&nbsp;&nbsp;&nbsp;"friendly_name": "ROG PG279Q"</b>
            <b>&nbsp;&nbsp;&nbsp;&nbsp;...</b>
            <b>&nbsp;&nbsp;}</b>
          </pre>
        </template>
        <template #linux>
          <pre style="white-space: pre-line">
            Info: Detecting displays
            Info: Detected display: DVI-D-0 (id: 0) connected: false
            Info: Detected display: HDMI-0 (id: 1) connected: true
            Info: Detected display: DP-0 (id: 2) connected: true
            Info: Detected display: DP-1 (id: 3) connected: false
            Info: Detected display: DVI-D-1 (id: 4) connected: false
          </pre>
        </template>
        <template #macos>
          <pre style="white-space: pre-line">
            Info: Detecting displays
            Info: Detected display: Monitor-0 (id: 3) connected: true
            Info: Detected display: Monitor-1 (id: 2) connected: true
          </pre>
        </template>
      </PlatformLayout>
    </div>
  </div>
</template>
