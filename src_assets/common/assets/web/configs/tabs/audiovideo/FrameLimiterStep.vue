<script setup lang="ts">
import { computed, onMounted, ref, watch } from 'vue';
import { useConfigStore } from '@/stores/config';
import { NSwitch, NSelect, NInput, NButton } from 'naive-ui';
import { http } from '@/http';
import { useI18n } from 'vue-i18n';

const props = defineProps<{ stepLabel: string }>();

const { t } = useI18n();
const store = useConfigStore();
const config = store.config;

const status = ref<any>(null);
const statusError = ref<string | null>(null);
const loading = ref(false);

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

const providerLabelFor = (id: string) => {
  switch (id) {
    case 'nvidia-control-panel':
      return t('frameLimiter.provider.nvcp');
    case 'rtss':
      return t('frameLimiter.provider.rtss');
    case 'none':
      return t('frameLimiter.provider.none');
    case 'auto':
    default:
      return t('frameLimiter.provider.auto');
  }
};

const providerOptions = computed(() => [
  { label: providerLabelFor('auto'), value: 'auto' },
  { label: providerLabelFor('nvidia-control-panel'), value: 'nvidia-control-panel' },
  { label: providerLabelFor('rtss'), value: 'rtss' },
]);

const syncLimiterOptions = computed(() => [
  { label: t('frameLimiter.syncLimiter.keep'), value: '' },
  { label: t('frameLimiter.syncLimiter.async'), value: 'async' },
  { label: t('frameLimiter.syncLimiter.front'), value: 'front edge sync' },
  { label: t('frameLimiter.syncLimiter.back'), value: 'back edge sync' },
  { label: t('frameLimiter.syncLimiter.reflex'), value: 'nvidia reflex' },
]);

const nvidiaDetected = computed(() => !!status.value?.nvidia_available);
const nvcpReady = computed(() => !!status.value?.nvcp_ready);
const nvOverridesSupported = computed(() => !!status.value?.nv_overrides_supported);
const rtssDetected = computed(() => {
  const s = status.value?.rtss;
  return !!(s && s.path_exists && s.hooks_found && s.profile_found);
});

const shouldShowRtssConfig = computed(() => {
  const provider = frameLimiterProvider.value;
  if (provider === 'rtss') {
    return true;
  }
  if (provider === 'auto' && !nvOverridesSupported.value) {
    return true;
  }
  return false;
});

const showRtssInstallHint = computed(() => shouldShowRtssConfig.value && !rtssDetected.value);

const statusBadgeClass = computed(() =>
  nvOverridesSupported.value || rtssDetected.value ? 'bg-success/10 text-success' : 'bg-warning/10 text-warning',
);

const statusIcon = computed(() =>
  nvOverridesSupported.value || rtssDetected.value ? 'fas fa-check-circle' : 'fas fa-exclamation-triangle',
);

const statusMessage = computed(() => status.value?.message || t('frameLimiter.status.unknown'));

watch(frameLimiterProvider, () => {
  refreshStatus();
});

watch(frameLimiterEnabled, () => {
  refreshStatus();
});

async function refreshStatus() {
  if (loading.value) return;
  loading.value = true;
  statusError.value = null;
  try {
    const res = await http.get('/api/rtss/status', { params: { _ts: Date.now() } });
    status.value = res?.data || null;
  } catch (e: any) {
    statusError.value = e?.message || t('frameLimiter.status.error');
  } finally {
    loading.value = false;
  }
}

function handleProviderDropdown(show: boolean) {
  if (show) {
    refreshStatus();
  }
}

onMounted(() => {
  refreshStatus();
});
</script>

<template>
  <fieldset class="border border-dark/35 dark:border-light/25 rounded-xl p-4">
    <legend class="px-2 text-sm font-medium">
      {{ stepLabel }}: {{ t('frameLimiter.stepTitle') }}
    </legend>

    <div class="space-y-4">
      <div v-if="status || statusError" :class="['rounded-lg px-4 py-3 text-[12px]', statusBadgeClass]">
        <div class="flex items-center justify-between gap-3">
          <div class="flex items-center gap-2">
            <i :class="statusIcon" />
            <span class="font-medium leading-tight">{{ statusMessage }}</span>
          </div>
          <n-button size="tiny" type="default" strong :loading="loading" @click="refreshStatus">
            <i class="fas fa-sync" />
            <span class="ml-1">{{ t('frameLimiter.actions.refresh') }}</span>
          </n-button>
        </div>
        <p v-if="statusError" class="mt-2 text-xs text-warning">{{ statusError }}</p>
      </div>

      <div class="grid gap-4 md:grid-cols-2">
        <div class="space-y-2">
          <label class="form-label" for="frame_limiter_enable">{{ t('frameLimiter.enable') }}</label>
          <n-switch id="frame_limiter_enable" v-model:value="frameLimiterEnabled" />
          <p class="form-text">{{ t('frameLimiter.enableHint') }}</p>
        </div>

        <div class="space-y-2">
          <label class="form-label" for="frame_limiter_provider">{{ t('frameLimiter.providerLabel') }}</label>
          <n-select
            id="frame_limiter_provider"
            v-model:value="frameLimiterProvider"
            :options="providerOptions"
            :data-search-options="providerOptions.map((o) => `${o.label}::${o.value}`).join('|')"
            @update:show="handleProviderDropdown"
          />
          <p class="form-text">{{ t('frameLimiter.providerHint') }}</p>
        </div>
      </div>

      <div class="space-y-2">
        <label class="form-label" for="disable_vsync_ullm">{{ t('frameLimiter.vsyncUllmLabel') }}</label>
        <n-switch id="disable_vsync_ullm" v-model:value="config.rtss_disable_vsync_ullm" />
        <p class="form-text">
          {{
            nvidiaDetected && nvcpReady
              ? t('frameLimiter.vsyncUllmHintNv')
              : t('frameLimiter.vsyncUllmHintGeneric')
          }}
        </p>
      </div>

      <div v-if="shouldShowRtssConfig" class="space-y-4">
        <div class="grid gap-4 md:grid-cols-2">
          <div>
            <label class="form-label" for="rtss_install_path">{{ t('frameLimiter.rtssPath') }}</label>
            <n-input
              id="rtss_install_path"
              v-model:value="config.rtss_install_path"
              :placeholder="t('frameLimiter.rtssPathPlaceholder')"
            />
            <p class="form-text">{{ t('frameLimiter.rtssPathHint') }}</p>
          </div>
          <div>
            <label class="form-label" for="rtss_frame_limit_type">{{ t('frameLimiter.syncLimiterLabel') }}</label>
            <n-select
              id="rtss_frame_limit_type"
              v-model:value="config.rtss_frame_limit_type"
              :options="syncLimiterOptions"
            />
            <p class="form-text">{{ t('frameLimiter.syncLimiterHint') }}</p>
          </div>
        </div>
        <p v-if="showRtssInstallHint" class="text-[11px] text-warning">
          {{ t('frameLimiter.rtssMissing') }}
        </p>
      </div>
    </div>
  </fieldset>
</template>

<style scoped></style>
