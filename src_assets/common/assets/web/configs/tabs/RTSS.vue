<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import { useConfigStore } from '@/stores/config';
import { storeToRefs } from 'pinia';
import { NSwitch, NSelect, NInput, NButton } from 'naive-ui';
import { http } from '@/http';

const store = useConfigStore();
const { metadata } = storeToRefs(store);
const config = store.config;
const platform = computed(() => metadata.value?.platform || '');

const syncLimiterOptions = [
  { label: 'Do not change', value: '' },
  { label: 'Async', value: 'async' },
  { label: 'Front edge sync', value: 'front edge sync' },
  { label: 'Back edge sync', value: 'back edge sync' },
  { label: 'NVIDIA Reflex', value: 'nvidia reflex' },
];

const status = ref<any>(null);
const statusError = ref<string | null>(null);

const isWindows = computed(() => platform.value === 'windows');
const detected = computed(() => {
  const s = status.value;
  return !!(s && s.path_exists && s.hooks_found && s.profile_found);
});
const limiterEnabled = computed(() => !!status.value?.enabled);
onMounted(async () => {
  try {
    const res = await http.get('/api/rtss/status', { params: { _ts: Date.now() } });
    status.value = res?.data || null;
  } catch (e: any) {
    statusError.value = e?.message || 'Failed to query RTSS status';
  }
});

async function refreshStatus() {
  statusError.value = null;
  try {
    const res = await http.get('/api/rtss/status', { params: { _ts: Date.now() } });
    status.value = res?.data || null;
  } catch (e: any) {
    statusError.value = e?.message || 'Failed to query RTSS status';
  }
}
</script>

<template>
  <div class="config-page">
    <div class="mb-2 text-[12px] opacity-70">
      Frame limiter integration via RTSS. Applies a frame limit at stream start and restores it when streaming stops.
    </div>

    <!-- Inline status row with refresh -->
    <div v-if="status || statusError" class="mb-4">
      <div
        :class="[
          'flex items-center justify-between gap-3 text-[12px] rounded px-3 py-2',
          detected ? 'bg-success/10 text-success' : 'bg-warning/10 text-warning',
        ]"
      >
        <div class="flex items-center gap-2">
          <i :class="detected ? 'fas fa-check-circle' : 'fas fa-exclamation-triangle'" />
          <div class="flex items-center gap-2 flex-wrap">
            <span class="font-medium">{{ detected ? 'RTSS detected' : 'RTSS not detected' }}</span>
            <span v-if="!detected && status" class="opacity-80">
              <template v-if="!status.path_exists">Install not found at <code>{{ status.resolved_path || '(none)' }}</code></template>
              <template v-else-if="!status.hooks_found">Hooks DLL missing</template>
              <template v-else-if="!status.profile_found">Global profile missing</template>
            </span>
            <span v-if="status && !limiterEnabled" class="opacity-70">(limiter disabled in settings)</span>
          </div>
        </div>
        <n-button size="tiny" type="default" strong @click="refreshStatus">
          <i class="fas fa-sync" />
          <span class="ml-1">Refresh</span>
        </n-button>
      </div>
      <div v-if="statusError" class="mt-2 text-[12px] text-warning">{{ statusError }}</div>
    </div>

    <div v-if="isWindows && status && detected && status.resolved_path" class="-mt-2 mb-4 text-[12px] opacity-60">
      Resolved path: <code>{{ status.resolved_path }}</code>
    </div>

    <div v-else-if="isWindows && status && !detected && status.resolved_path" class="-mt-2 mb-4 text-[12px] opacity-60">
      Attempted path: <code>{{ status.resolved_path }}</code>
    </div>

    <div v-else-if="statusError && !status" class="mb-4 text-[12px] text-warning">
      {{ statusError }}
    </div>

    <!-- Install Path: only show when not detected -->
    <div v-if="!detected" class="mb-6">
      <label for="rtss_install_path" class="form-label">RTSS install path</label>
      <n-input
        id="rtss_install_path"
        v-model:value="config.rtss_install_path"
        placeholder="C:\Program Files (x86)\RivaTuner Statistics Server"
      />
      <div class="form-text">
        Root install folder (leave blank to auto-detect under Program Files / Program Files (x86)).
      </div>
    </div>

    <!-- Enable Frame Limiter: only when RTSS detected -->
    <div v-if="detected" class="mb-6">
      <label for="rtss_enable_frame_limit" class="form-label">Enable frame limiter</label>
      <div class="flex items-center gap-3">
        <n-switch id="rtss_enable_frame_limit" v-model:value="config.rtss_enable_frame_limit" />
      </div>
      <div class="form-text">
        When enabled, Sunshine will set RTSS Global profile to limit framerate to the client FPS during a session, then restore previous values at the end.
      </div>
    </div>

    <!-- SyncLimiter Type: only when RTSS detected -->
    <div v-if="detected" class="mb-6">
      <label for="rtss_frame_limit_type" class="form-label">SyncLimiter mode</label>
      <n-select
        id="rtss_frame_limit_type"
        v-model:value="config.rtss_frame_limit_type"
        :options="syncLimiterOptions"
        :data-search-options="syncLimiterOptions.map(o => `${o.label}::${o.value}`).join('|')"
      />
      <div class="form-text">
        Advanced users only. Async is recommended for best performance; other modes may cause microstuttering. If set, Sunshine will also adjust RTSS SyncLimiter for the Global profile.
      </div>
    </div>
  </div>
  
</template>

<style scoped></style>
