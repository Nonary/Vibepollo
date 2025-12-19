<script setup lang="ts">
import { computed, onMounted, ref, watch } from 'vue';
import { useConfigStore } from '@/stores/config';
import { NSwitch, NSelect, NInput, NInputNumber, NButton, NTable } from 'naive-ui';
import { http } from '@/http';
import { useI18n } from 'vue-i18n';

const props = defineProps<{ stepLabel: string }>();

const { t } = useI18n();
const store = useConfigStore();
const config = store.config;
const dummyPlugHdrActive = computed(() => !!config.dd_wa_dummy_plug_hdr10);

watch(
  () => config.dd_wa_dummy_plug_hdr10,
  (value) => {
    if (value && !config.frame_limiter_disable_vsync) {
      config.frame_limiter_disable_vsync = true;
    }
  },
  { immediate: true },
);

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
  { label: providerLabelFor('rtss'), value: 'rtss' },
  { label: providerLabelFor('nvidia-control-panel'), value: 'nvidia-control-panel' },
]);

const syncLimiterOptions = computed(() => [
  { label: t('frameLimiter.syncLimiter.keep'), value: '' },
  { label: t('frameLimiter.syncLimiter.async'), value: 'async' },
  { label: t('frameLimiter.syncLimiter.front'), value: 'front edge sync' },
  { label: t('frameLimiter.syncLimiter.back'), value: 'back edge sync' },
  { label: t('frameLimiter.syncLimiter.reflex'), value: 'nvidia reflex' },
]);

const syncLimiterHelpRows = computed(() => [
  {
    id: 'async',
    label: t('rtss.sync_limiter_async_short'),
    latency: t('rtss.sync_limiter_async_latency'),
    stutter: t('rtss.sync_limiter_async_stutter'),
    advantages: t('rtss.sync_limiter_async_advantages'),
    disadvantages: t('rtss.sync_limiter_async_disadvantages'),
    use: t('rtss.sync_limiter_async_use'),
  },
  {
    id: 'front',
    label: t('rtss.sync_limiter_front_short'),
    latency: t('rtss.sync_limiter_front_latency'),
    stutter: t('rtss.sync_limiter_front_stutter'),
    advantages: t('rtss.sync_limiter_front_advantages'),
    disadvantages: t('rtss.sync_limiter_front_disadvantages'),
    use: t('rtss.sync_limiter_front_use'),
  },
  {
    id: 'back',
    label: t('rtss.sync_limiter_back_short'),
    latency: t('rtss.sync_limiter_back_latency'),
    stutter: t('rtss.sync_limiter_back_stutter'),
    advantages: t('rtss.sync_limiter_back_advantages'),
    disadvantages: t('rtss.sync_limiter_back_disadvantages'),
    use: t('rtss.sync_limiter_back_use'),
  },
  {
    id: 'reflex',
    label: t('rtss.sync_limiter_reflex_short'),
    latency: t('rtss.sync_limiter_reflex_latency'),
    stutter: t('rtss.sync_limiter_reflex_stutter'),
    advantages: t('rtss.sync_limiter_reflex_advantages'),
    disadvantages: t('rtss.sync_limiter_reflex_disadvantages'),
    use: t('rtss.sync_limiter_reflex_use'),
  },
]);

const nvidiaDetected = computed(() => !!status.value?.nvidia_available);
const nvcpReady = computed(() => !!status.value?.nvcp_ready);
const rtssDetected = computed(() => {
  const s = status.value;
  return !!(s && s.path_exists && s.hooks_found);
});

const effectiveProvider = computed(() => {
  const active = status.value?.active_provider;
  if (active && active !== 'none' && active !== 'auto') {
    return active;
  }

  const provider = frameLimiterProvider.value;
  if (provider === 'auto') {
    if (status.value?.rtss_available || rtssDetected.value) {
      return 'rtss';
    }
    if (nvcpReady.value && nvidiaDetected.value) {
      return 'nvidia-control-panel';
    }
    return 'rtss';
  }
  return provider;
});

const rtssBootstrapPending = computed(() => {
  const s = status.value;
  return !!(s && s.can_bootstrap_profile && !s.profile_found);
});

const rtssAutoLaunchPlanned = computed(() => {
  const s = status.value;
  return !!(s && s.path_exists && s.hooks_found && !s.process_running);
});

const shouldShowRtssConfig = computed(() => {
  const provider = frameLimiterProvider.value;
  return provider === 'rtss' || provider === 'auto';
});

const showRtssInstallHint = computed(() => shouldShowRtssConfig.value && !rtssDetected.value);

const showRtssInstallInput = computed(() => shouldShowRtssConfig.value && !rtssDetected.value);

const showSyncLimiterSelect = computed(() => {
  const provider = frameLimiterProvider.value;
  if (provider === 'rtss') {
    return true;
  }
  if (provider === 'auto') {
    return effectiveProvider.value === 'rtss';
  }
  return false;
});

const showSyncLimiterHelp = computed(() => showSyncLimiterSelect.value);

const statusBadgeClass = computed(() => {
  if (!status.value || !frameLimiterEnabled.value) {
    return 'bg-warning/10 text-warning';
  }
  if (effectiveProvider.value === 'nvidia-control-panel') {
    return nvidiaDetected.value && nvcpReady.value
      ? 'bg-success/10 text-success'
      : 'bg-warning/10 text-warning';
  }
  if (effectiveProvider.value === 'rtss') {
    return rtssDetected.value || rtssBootstrapPending.value
      ? 'bg-success/10 text-success'
      : 'bg-warning/10 text-warning';
  }
  return 'bg-warning/10 text-warning';
});

const statusIcon = computed(() =>
  statusBadgeClass.value.includes('bg-success')
    ? 'fas fa-check-circle'
    : 'fas fa-exclamation-triangle',
);

const statusMessage = computed(() => {
  if (!status.value) {
    return t('frameLimiter.status.unknown');
  }
  if (!frameLimiterEnabled.value) {
    return t('frameLimiter.status.limiterDisabled');
  }
  if (effectiveProvider.value === 'nvidia-control-panel') {
    if (!nvidiaDetected.value) {
      return t('frameLimiter.status.nvcpNotDetected');
    }
    if (!nvcpReady.value) {
      return t('frameLimiter.status.nvcpUnavailable');
    }
    return t('frameLimiter.status.nvcpDetected');
  }
  if (effectiveProvider.value === 'rtss') {
    if (rtssDetected.value) {
      return t('frameLimiter.status.rtssDetected');
    }
    if (rtssBootstrapPending.value) {
      return t('frameLimiter.status.rtssBootstrap');
    }
    return t('frameLimiter.status.rtssNotDetected');
  }
  return t('frameLimiter.status.unknown');
});

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

    <div class="mb-4 rounded-lg border border-primary/30 bg-primary/10 px-4 py-3 text-[12px]">
      <div class="font-medium">{{ t('frameLimiter.noticeTitle') }}</div>
      <div class="mt-1 opacity-80">{{ t('frameLimiter.noticeCopy') }}</div>
    </div>

    <div class="space-y-4">
      <div
        v-if="status || statusError"
        :class="['rounded-lg px-4 py-3 text-[12px]', statusBadgeClass]"
      >
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
        <p
          v-if="
            status &&
            effectiveProvider === 'rtss' &&
            (rtssBootstrapPending || rtssAutoLaunchPlanned)
          "
          class="mt-2 text-xs opacity-80"
        >
          <span v-if="rtssBootstrapPending">{{ t('frameLimiter.status.rtssBootstrapHint') }}</span>
          <span v-else-if="rtssAutoLaunchPlanned">{{
            t('frameLimiter.status.rtssAutolaunchHint')
          }}</span>
        </p>
        <p v-if="statusError" class="mt-2 text-xs text-warning">{{ statusError }}</p>
      </div>

      <div class="grid gap-4 md:grid-cols-2">
        <div class="space-y-2">
          <label class="form-label" for="frame_limiter_enable">{{
            t('frameLimiter.enable')
          }}</label>
          <n-switch id="frame_limiter_enable" v-model:value="frameLimiterEnabled" />
          <p class="form-text">{{ t('frameLimiter.enableHint') }}</p>
        </div>

        <div class="space-y-2">
          <label class="form-label" for="frame_limiter_provider">{{
            t('frameLimiter.providerLabel')
          }}</label>
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
        <label class="form-label" for="frame_limiter_fps_limit">{{
          t('frameLimiter.limitLabel')
        }}</label>
        <n-input-number
          id="frame_limiter_fps_limit"
          v-model:value="config.frame_limiter_fps_limit"
          :min="0"
          :max="1000"
          :step="1"
          :precision="0"
          :placeholder="t('frameLimiter.limitPlaceholder')"
        />
        <p class="form-text">{{ t('frameLimiter.limitHint') }}</p>
      </div>

      <div class="space-y-2">
        <label class="form-label" for="frame_limiter_disable_vsync">{{
          t('frameLimiter.vsyncUllmLabel')
        }}</label>
        <n-switch
          id="frame_limiter_disable_vsync"
          v-model:value="config.frame_limiter_disable_vsync"
          :disabled="dummyPlugHdrActive"
        />
        <p class="form-text">
          {{
            dummyPlugHdrActive
              ? t('frameLimiter.vsyncUllmForcedByDummyPlug')
              : nvidiaDetected && nvcpReady
                ? t('frameLimiter.vsyncUllmHintNv')
                : t('frameLimiter.vsyncUllmHintGeneric')
          }}
        </p>
      </div>

      <div v-if="shouldShowRtssConfig" class="space-y-4">
        <div v-if="showRtssInstallInput" class="space-y-2">
          <label class="form-label" for="rtss_install_path">{{ t('frameLimiter.rtssPath') }}</label>
          <n-input
            id="rtss_install_path"
            v-model:value="config.rtss_install_path"
            :placeholder="t('frameLimiter.rtssPathPlaceholder')"
          />
          <p class="form-text">{{ t('frameLimiter.rtssPathHint') }}</p>
        </div>
        <p v-if="showRtssInstallHint" class="text-[11px] text-warning">
          {{ t('frameLimiter.rtssMissing') }}
        </p>
      </div>

      <div
        v-if="showSyncLimiterHelp"
        class="rounded-lg border border-primary/30 bg-primary/5 p-4 text-[12px]"
      >
        <div class="text-[13px] font-medium">{{ t('rtss.sync_limiter_help_heading') }}</div>
        <div class="mt-1 opacity-80">{{ t('rtss.sync_limiter_help_blurb') }}</div>
        <div class="mt-3 desktop-sync-table">
          <div class="overflow-x-auto">
            <n-table
              size="small"
              :single-line="false"
              :bordered="false"
              class="min-w-full text-left whitespace-normal break-words"
            >
              <thead>
                <tr
                  class="border-b border-primary/30 text-[11px] uppercase tracking-wide opacity-70"
                >
                  <th scope="col" class="pb-2 pr-4 font-medium">
                    {{ t('rtss.sync_limiter_help_mode') }}
                  </th>
                  <th scope="col" class="pb-2 pr-4 font-medium">
                    {{ t('rtss.sync_limiter_help_latency') }}
                  </th>
                  <th scope="col" class="pb-2 pr-4 font-medium">
                    {{ t('rtss.sync_limiter_help_stutter') }}
                  </th>
                  <th scope="col" class="pb-2 pr-4 font-medium">
                    {{ t('rtss.sync_limiter_help_advantages') }}
                  </th>
                  <th scope="col" class="pb-2 pr-4 font-medium">
                    {{ t('rtss.sync_limiter_help_disadvantages') }}
                  </th>
                  <th scope="col" class="pb-2 font-medium">
                    {{ t('rtss.sync_limiter_help_usage') }}
                  </th>
                </tr>
              </thead>
              <tbody>
                <tr
                  v-for="row in syncLimiterHelpRows"
                  :key="row.id"
                  class="border-b border-primary/20 last:border-0"
                >
                  <th scope="row" class="py-3 pr-4 text-[12px] font-medium align-top">
                    <span class="font-semibold">{{ row.label }}</span>
                  </th>
                  <td class="py-3 pr-4 align-top text-[12px]">{{ row.latency }}</td>
                  <td class="py-3 pr-4 align-top text-[12px]">{{ row.stutter }}</td>
                  <td class="py-3 pr-4 align-top text-[12px]">{{ row.advantages }}</td>
                  <td class="py-3 pr-4 align-top text-[12px]">{{ row.disadvantages }}</td>
                  <td class="py-3 align-top text-[12px]">{{ row.use }}</td>
                </tr>
              </tbody>
            </n-table>
          </div>
        </div>
        <div class="mt-3 space-y-3 mobile-sync-list">
          <div
            v-for="row in syncLimiterHelpRows"
            :key="row.id"
            class="rounded-lg border border-primary/20 bg-primary/10 p-3"
          >
            <div class="text-[13px] font-semibold">{{ row.label }}</div>
            <dl class="mt-2 space-y-2">
              <div>
                <dt class="text-[11px] uppercase tracking-wide opacity-70">
                  {{ t('rtss.sync_limiter_help_latency') }}
                </dt>
                <dd class="text-[12px] leading-snug">{{ row.latency }}</dd>
              </div>
              <div>
                <dt class="text-[11px] uppercase tracking-wide opacity-70">
                  {{ t('rtss.sync_limiter_help_stutter') }}
                </dt>
                <dd class="text-[12px] leading-snug">{{ row.stutter }}</dd>
              </div>
              <div>
                <dt class="text-[11px] uppercase tracking-wide opacity-70">
                  {{ t('rtss.sync_limiter_help_advantages') }}
                </dt>
                <dd class="text-[12px] leading-snug">{{ row.advantages }}</dd>
              </div>
              <div>
                <dt class="text-[11px] uppercase tracking-wide opacity-70">
                  {{ t('rtss.sync_limiter_help_disadvantages') }}
                </dt>
                <dd class="text-[12px] leading-snug">{{ row.disadvantages }}</dd>
              </div>
              <div>
                <dt class="text-[11px] uppercase tracking-wide opacity-70">
                  {{ t('rtss.sync_limiter_help_usage') }}
                </dt>
                <dd class="text-[12px] leading-snug">{{ row.use }}</dd>
              </div>
            </dl>
          </div>
        </div>
        <div v-if="showSyncLimiterSelect" class="mt-4 space-y-2">
          <label class="form-label" for="rtss_frame_limit_type">{{
            t('frameLimiter.syncLimiterLabel')
          }}</label>
          <n-select
            id="rtss_frame_limit_type"
            v-model:value="config.rtss_frame_limit_type"
            :options="syncLimiterOptions"
          />
          <p class="form-text">{{ t('frameLimiter.syncLimiterHint') }}</p>
        </div>
      </div>
    </div>
  </fieldset>
</template>

<style scoped>
.desktop-sync-table {
  display: none;
}

.mobile-sync-list {
  display: flex;
  flex-direction: column;
}

@media (min-width: 768px) {
  .desktop-sync-table {
    display: block;
  }

  .mobile-sync-list {
    display: none;
  }
}
</style>
