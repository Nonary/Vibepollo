<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import { useConfigStore } from '@/stores/config';
import { storeToRefs } from 'pinia';
import { NSwitch, NSelect, NInput, NButton } from 'naive-ui';
import { http } from '@/http';
import { useI18n } from 'vue-i18n';

const { t } = useI18n();

const store = useConfigStore();
const { metadata } = storeToRefs(store);
const config = store.config;
const platform = computed(() => metadata.value?.platform || '');

const syncLimiterOptions = computed(() => [
  { label: t('rtss.sync_limiter_do_not_change'), value: '' },
  { label: t('rtss.sync_limiter_async'), value: 'async' },
  { label: t('rtss.sync_limiter_front_edge'), value: 'front edge sync' },
  { label: t('rtss.sync_limiter_back_edge'), value: 'back edge sync' },
  { label: t('rtss.sync_limiter_reflex'), value: 'nvidia reflex' },
]);

const providerOptions = computed(() => [
  { label: t('rtss.provider_auto'), value: 'auto' },
  { label: t('rtss.provider_nvcp'), value: 'nvidia-control-panel' },
  { label: t('rtss.provider_rtss'), value: 'rtss' },
]);

const status = ref<any>(null);
const statusError = ref<string | null>(null);

const isWindows = computed(() => platform.value === 'windows');

const frameLimiterEnabled = computed({
  get: () => !!config.frame_limiter_enable,
  set: (value: boolean) => {
    config.frame_limiter_enable = value;
  },
});

const frameLimiterProvider = computed({
  get: () => config.frame_limiter_provider || 'auto',
  set: (value: string) => {
    config.frame_limiter_provider = value;
  },
});

const rtssDetected = computed(() => {
  const s = status.value;
  return !!(s && s.path_exists && s.hooks_found && s.profile_found);
});

const nvidiaDetected = computed(() => !!status.value?.nvidia_available);
const nvcpReady = computed(() => !!status.value?.nvcp_ready);
const activeProvider = computed(() => status.value?.active_provider || '');
const configuredProvider = computed(
  () => status.value?.configured_provider || frameLimiterProvider.value,
);

const preferRtss = computed(() => {
  const provider = frameLimiterProvider.value;
  if (provider === 'rtss') {
    return true;
  }
  if (provider === 'auto') {
    return !nvcpReady.value;
  }
  return false;
});

const shouldShowRtssConfig = computed(() => preferRtss.value);

const statusHealthy = computed(() => {
  if (!status.value || !status.value.enabled || !frameLimiterEnabled.value) {
    return false;
  }
  if (activeProvider.value === 'nvidia-control-panel') {
    return true;
  }
  const provider = configuredProvider.value;

  if (provider === 'nvidia-control-panel') {
    return nvcpReady.value && nvidiaDetected.value;
  }
  if (provider === 'auto') {
    if (nvcpReady.value && nvidiaDetected.value) {
      return true;
    }
    if (preferRtss.value) {
      return rtssDetected.value;
    }
  }
  if (provider === 'rtss' || preferRtss.value) {
    return rtssDetected.value;
  }
  return false;
});

const statusIcon = computed(() =>
  statusHealthy.value ? 'fas fa-check-circle' : 'fas fa-exclamation-triangle',
);
const statusBadgeClass = computed(() =>
  statusHealthy.value ? 'bg-success/10 text-success' : 'bg-warning/10 text-warning',
);
const statusMessage = computed(() => status.value?.message || t('rtss.status_unknown'));

const providerLabel = (id: string): string => {
  switch (id) {
    case 'auto':
      return t('rtss.provider_auto');
    case 'rtss':
      return t('rtss.provider_rtss');
    case 'nvidia-control-panel':
      return t('rtss.provider_nvcp');
    case 'none':
      return t('rtss.provider_none');
    default:
      return id || t('rtss.provider_unknown');
  }
};

const activeProviderLabel = computed(() => providerLabel(activeProvider.value));
const configuredProviderLabel = computed(() => providerLabel(configuredProvider.value));

const showRtssPath = computed(() => shouldShowRtssConfig.value && status.value?.resolved_path);

onMounted(async () => {
  try {
    const res = await http.get('/api/rtss/status', { params: { _ts: Date.now() } });
    status.value = res?.data || null;
  } catch (e: any) {
    statusError.value = e?.message || t('rtss.error_query_status');
  }
});

async function refreshStatus() {
  statusError.value = null;
  try {
    const res = await http.get('/api/rtss/status', { params: { _ts: Date.now() } });
    status.value = res?.data || null;
  } catch (e: any) {
    statusError.value = e?.message || t('rtss.error_query_status');
  }
}
</script>

<template>
  <div class="config-page">
    <div class="mb-2 text-[12px] opacity-70">
      {{ $t('rtss.desc') }}
    </div>

    <!-- Inline status row with refresh -->
    <div v-if="status || statusError" class="mb-4">
      <div
        :class="[
          'flex items-start justify-between gap-3 text-[12px] rounded px-3 py-2',
          statusBadgeClass,
        ]"
      >
        <div class="flex flex-col gap-1 flex-1">
          <div class="flex items-center gap-2">
            <i :class="statusIcon" />
            <span class="font-medium">{{ statusMessage }}</span>
          </div>
          <div v-if="status" class="flex flex-wrap gap-x-4 gap-y-1 text-[11px] opacity-80">
            <span>{{
              $t('rtss.status_configured_provider', { provider: configuredProviderLabel })
            }}</span>
            <span>{{ $t('rtss.status_active_provider', { provider: activeProviderLabel }) }}</span>
          </div>
        </div>
        <n-button size="tiny" type="default" strong @click="refreshStatus">
          <i class="fas fa-sync" />
          <span class="ml-1">{{ $t('rtss.refresh') }}</span>
        </n-button>
      </div>
      <div v-if="statusError" class="mt-2 text-[12px] text-warning">{{ statusError }}</div>
    </div>

    <div v-if="showRtssPath" class="-mt-2 mb-4 text-[12px] opacity-60">
      <template v-if="rtssDetected">{{ $t('rtss.resolved_path') }}</template>
      <template v-else>{{ $t('rtss.attempted_path') }}</template>
      <code class="ml-1">{{ status.resolved_path }}</code>
    </div>

    <div v-else-if="statusError && !status" class="mb-4 text-[12px] text-warning">
      {{ statusError }}
    </div>

    <div v-if="isWindows" class="mb-6">
      <label for="frame_limiter_provider" class="form-label">{{ $t('rtss.provider_label') }}</label>
      <n-select
        id="frame_limiter_provider"
        v-model:value="frameLimiterProvider"
        :options="providerOptions"
        :data-search-options="providerOptions.map((o) => `${o.label}::${o.value}`).join('|')"
      />
      <div class="form-text">{{ $t('rtss.provider_desc') }}</div>
    </div>

    <div class="mb-6">
      <label for="frame_limiter_enable" class="form-label">{{
        $t('rtss.enable_frame_limiter')
      }}</label>
      <div class="flex items-center gap-3">
        <n-switch id="frame_limiter_enable" v-model:value="frameLimiterEnabled" />
      </div>
      <div class="form-text">{{ $t('rtss.enable_frame_limiter_desc') }}</div>
    </div>

    <!-- Install Path: only show when RTSS is the active provider -->
    <div v-if="shouldShowRtssConfig" class="mb-6">
      <label for="rtss_install_path" class="form-label">{{ $t('rtss.install_path') }}</label>
      <n-input
        id="rtss_install_path"
        v-model:value="config.rtss_install_path"
        :placeholder="$t('rtss.install_path_placeholder')"
      />
      <div class="form-text">{{ $t('rtss.install_path_desc') }}</div>
    </div>

    <!-- SyncLimiter Type: only when RTSS is active -->
    <div v-if="shouldShowRtssConfig" class="mb-6">
      <label for="rtss_frame_limit_type" class="form-label">{{
        $t('rtss.sync_limiter_mode')
      }}</label>
      <n-select
        id="rtss_frame_limit_type"
        v-model:value="config.rtss_frame_limit_type"
        :options="syncLimiterOptions"
        :data-search-options="syncLimiterOptions.map((o) => `${o.label}::${o.value}`).join('|')"
      />
      <div class="form-text">{{ $t('rtss.sync_limiter_desc') }}</div>
    </div>

    <!-- Disable VSYNC/ULLM by preferring highest refresh rate -->
    <div class="mb-6">
      <label for="rtss_disable_vsync_ullm" class="form-label">{{
        $t('rtss.rtss_disable_vsync_ullm')
      }}</label>
      <div class="flex items-center gap-3">
        <n-switch id="rtss_disable_vsync_ullm" v-model:value="config.rtss_disable_vsync_ullm" />
      </div>
      <div class="form-text">{{ $t('rtss.rtss_disable_vsync_ullm_desc') }}</div>
    </div>
  </div>
</template>

<style scoped></style>
