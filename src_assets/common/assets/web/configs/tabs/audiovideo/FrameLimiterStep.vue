<script setup lang="ts">
import { computed, onMounted, ref, watch } from 'vue';
import ConfigFieldRenderer from '@/ConfigFieldRenderer.vue';
import { useConfigStore } from '@/stores/config';
import { NButton, NTable } from 'naive-ui';
import { http } from '@/http';
import { useI18n } from 'vue-i18n';

defineProps<{ stepLabel: string }>();

const { t } = useI18n();
const store = useConfigStore();
const config = store.config;
const dummyPlugHdrActive = computed(() => !!config.dd_wa_dummy_plug_hdr10);

// Mirror the virtual-display detection used in DisplayDeviceOptions so the capture-mode copy
// appears only when a virtual screen is selected.
const VIRTUAL_DISPLAY_SELECTION = 'sunshine:virtual_display';
const usingVirtualDisplay = computed(() => {
  const mode = config.virtual_display_mode;
  if (mode === 'per_client' || mode === 'shared') {
    return true;
  }
  if (mode === 'disabled') {
    return false;
  }
  return config.output_name === VIRTUAL_DISPLAY_SELECTION;
});

const syncLimiterHint = computed(() =>
  usingVirtualDisplay.value
    ? t('frameLimiter.syncLimiterHintVirtual')
    : t('frameLimiter.syncLimiterHint'),
);

watch(
  () => config.dd_wa_dummy_plug_hdr10,
  (value) => {
    if (value && !config.frame_limiter_disable_vsync) {
      config.frame_limiter_disable_vsync = true;
    }
  },
  { immediate: true },
);

// Shape of /api/rtss/status (see confighttp_rtss.cpp); only the fields this step reads.
interface FrameLimiterStatus {
  enabled?: boolean;
  configured_provider?: string;
  active_provider?: string;
  nvidia_available?: boolean;
  nvcp_ready?: boolean;
  rtss_available?: boolean;
  path_exists?: boolean;
  hooks_found?: boolean;
  process_running?: boolean;
  can_bootstrap_profile?: boolean;
  profile_found?: boolean;
}

const status = ref<FrameLimiterStatus>();
const statusError = ref<string>();
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

type VirtualCaptureMode = 'enabled' | 'disabled' | 'legacy';

function normalizeVirtualCaptureMode(value: unknown): VirtualCaptureMode {
  const normalized = String(value ?? '')
    .toLowerCase()
    .trim();
  if (normalized === 'legacy' || normalized === '2x' || normalized === 'fixed-2x') {
    return 'legacy';
  }
  if (
    value === false ||
    value === 0 ||
    ['false', 'no', 'disable', 'disabled', 'off', '0'].includes(normalized)
  ) {
    return 'disabled';
  }
  return 'enabled';
}

const virtualCaptureMode = computed<VirtualCaptureMode>({
  get: () => normalizeVirtualCaptureMode(config.frame_limiter_auto_virtual_framegen),
  set: (value) => {
    config.frame_limiter_auto_virtual_framegen = value;
  },
});

const virtualCaptureSummary = computed(() => {
  switch (virtualCaptureMode.value) {
    case 'disabled':
      return t('frameLimiter.virtual.summaryDisabled');
    case 'legacy':
      return t('frameLimiter.virtual.summaryLegacy');
    default:
      return t('frameLimiter.virtual.summaryEnabled');
  }
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
const rtssUsable = computed(() => !!status.value?.rtss_available || rtssDetected.value);
const nvDriverFallbackReady = computed(() => nvidiaDetected.value && nvcpReady.value);

const rtssMissingText = computed(() =>
  nvDriverFallbackReady.value
    ? t('frameLimiter.rtssMissingNvFallback')
    : t('frameLimiter.rtssMissingNoFallback'),
);

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

// Wait for the status probe before claiming RTSS is missing so the hint doesn't flash
// (with the wrong fallback copy) while the page loads.
const showRtssInstallHint = computed(
  () => shouldShowRtssConfig.value && !!status.value && !rtssDetected.value,
);

const showRtssInstallInput = computed(
  () => shouldShowRtssConfig.value && !!status.value && !rtssDetected.value,
);

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

const autoVirtualLimiter = computed(() => virtualCaptureMode.value !== 'disabled');

// The auto-limit policy forces the limiter on for virtual screens even when the global
// toggle is off, so treat that combination as healthy rather than warning about it.
const virtualAutoCapCoversDisabledLimiter = computed(
  () =>
    usingVirtualDisplay.value &&
    autoVirtualLimiter.value &&
    (rtssUsable.value || nvDriverFallbackReady.value),
);

const statusBadgeClass = computed(() => {
  if (!status.value) {
    return 'bg-warning/10 text-warning';
  }
  if (!frameLimiterEnabled.value) {
    return virtualAutoCapCoversDisabledLimiter.value
      ? 'bg-success/10 text-success'
      : 'bg-warning/10 text-warning';
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
    return virtualAutoCapCoversDisabledLimiter.value
      ? t('frameLimiter.status.limiterDisabledVirtualAuto')
      : t('frameLimiter.status.limiterDisabled');
  }
  if (effectiveProvider.value === 'nvidia-control-panel') {
    if (!nvidiaDetected.value) {
      return t('frameLimiter.status.nvcpNotDetected');
    }
    if (!nvcpReady.value) {
      return t('frameLimiter.status.nvcpUnavailable');
    }
    // Distinguish "you picked the NVIDIA driver" from "Auto fell back because RTSS is missing".
    return frameLimiterProvider.value === 'auto' && !rtssUsable.value
      ? t('frameLimiter.status.nvcpFallback')
      : t('frameLimiter.status.nvcpDetected');
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
  void refreshStatus();
});

watch(frameLimiterEnabled, () => {
  void refreshStatus();
});

async function refreshStatus() {
  if (loading.value) return;
  loading.value = true;
  statusError.value = undefined;
  try {
    const res = await http.get<FrameLimiterStatus>('/api/rtss/status', {
      params: { _ts: Date.now() },
    });
    status.value = res?.data ?? undefined;
  } catch (e) {
    statusError.value = e instanceof Error ? e.message : t('frameLimiter.status.error');
  } finally {
    loading.value = false;
  }
}

function handleProviderDropdown(show: boolean) {
  if (show) {
    void refreshStatus();
  }
}

onMounted(() => {
  void refreshStatus();
});
</script>

<template>
  <section class="min-w-0">
    <h4 class="mb-3 break-words text-sm font-semibold leading-snug">
      {{ stepLabel }}: {{ t('frameLimiter.stepTitle') }}
    </h4>

    <div
      v-if="usingVirtualDisplay"
      class="mb-4 rounded-lg border border-primary/30 bg-primary/10 px-4 py-4 text-[12px]"
    >
      <div class="flex items-start gap-3">
        <i class="fas fa-tachometer-alt mt-0.5 text-[15px] opacity-80" />
        <div>
          <div class="text-[13px] font-semibold">{{ t('frameLimiter.virtual.title') }}</div>
          <p class="mt-1 leading-relaxed opacity-80">{{ virtualCaptureSummary }}</p>
        </div>
      </div>
    </div>
    <div
      v-else
      class="mb-4 rounded-lg border border-primary/30 bg-primary/10 px-4 py-3 text-[12px]"
    >
      <div class="font-medium">{{ t('frameLimiter.noticeTitle') }}</div>
      <div class="mt-1 opacity-80">{{ t('frameLimiter.noticeCopy') }}</div>
    </div>

    <div class="space-y-4">
      <div
        v-if="status || statusError"
        :class="['rounded-lg px-4 py-3 text-[12px]', statusBadgeClass]"
      >
        <div class="flex items-start justify-between gap-3">
          <div class="flex min-w-0 items-start gap-2">
            <i :class="statusIcon" />
            <span class="font-medium leading-tight">{{ statusMessage }}</span>
          </div>
          <n-button
            size="tiny"
            type="default"
            strong
            class="flex-none"
            :loading="loading"
            :aria-label="t('frameLimiter.actions.refresh')"
            @click="refreshStatus"
          >
            <i class="fas fa-sync" />
            <span class="ml-1 hidden sm:inline">{{ t('frameLimiter.actions.refresh') }}</span>
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
        <ConfigFieldRenderer
          v-model="frameLimiterEnabled"
          setting-key="frame_limiter_enable"
          :label="t('frameLimiter.enable')"
          :desc="t('frameLimiter.enableHint')"
        />

        <ConfigFieldRenderer
          v-model="frameLimiterProvider"
          setting-key="frame_limiter_provider"
          :label="t('frameLimiter.providerLabel')"
          :desc="t('frameLimiter.providerHint')"
          :options="providerOptions"
          @update:show="handleProviderDropdown"
        />
      </div>

      <ConfigFieldRenderer
        v-model="config.frame_limiter_fps_limit"
        setting-key="frame_limiter_fps_limit"
        :label="t('frameLimiter.limitLabel')"
        :desc="t('frameLimiter.limitHint')"
        :placeholder="t('frameLimiter.limitPlaceholder')"
      />

      <ConfigFieldRenderer
        v-model="virtualCaptureMode"
        setting-key="frame_limiter_auto_virtual_framegen"
        kind="select"
        :label="t('frameLimiter.autoVirtualFramegenLabel')"
        :desc="t('frameLimiter.autoVirtualFramegenHint')"
      />

      <ConfigFieldRenderer
        v-model="config.frame_limiter_disable_vsync"
        setting-key="frame_limiter_disable_vsync"
        :label="t('frameLimiter.vsyncUllmLabel')"
        :desc="
          dummyPlugHdrActive
            ? t('frameLimiter.vsyncUllmForcedByDummyPlug')
            : nvidiaDetected && nvcpReady
              ? t('frameLimiter.vsyncUllmHintNv')
              : t('frameLimiter.vsyncUllmHintGeneric')
        "
        :disabled="dummyPlugHdrActive"
      />

      <div v-if="shouldShowRtssConfig" class="space-y-4">
        <ConfigFieldRenderer
          v-if="showRtssInstallInput"
          v-model="config.rtss_install_path"
          setting-key="rtss_install_path"
          :label="t('frameLimiter.rtssPath')"
          :desc="t('frameLimiter.rtssPathHint')"
          :placeholder="t('frameLimiter.rtssPathPlaceholder')"
        />
        <p v-if="showRtssInstallHint" class="text-[11px] text-warning">
          {{ rtssMissingText }}
        </p>
      </div>

      <div
        v-if="showSyncLimiterHelp"
        class="rounded-lg bg-primary/5 p-3 text-[12px] sm:p-4"
      >
        <ConfigFieldRenderer
          v-if="showSyncLimiterSelect"
          v-model="config.rtss_frame_limit_type"
          setting-key="rtss_frame_limit_type"
          :label="t('frameLimiter.syncLimiterLabel')"
          :desc="syncLimiterHint"
          :options="syncLimiterOptions"
        />

        <details class="sync-comparison mt-3 border-t border-primary/20 pt-3">
          <summary class="flex cursor-pointer list-none items-center justify-between gap-3 py-1">
            <span class="text-[13px] font-medium">{{ t('rtss.sync_limiter_help_heading') }}</span>
            <i class="fas fa-chevron-down text-[11px] opacity-60 transition-transform" />
          </summary>
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
          <div class="mt-3 space-y-2 mobile-sync-list">
            <details
              v-for="row in syncLimiterHelpRows"
              :key="row.id"
              class="mode-card rounded-lg border border-primary/20 bg-primary/10"
            >
              <summary
                class="flex cursor-pointer list-none items-center justify-between gap-3 px-3 py-2.5"
              >
                <span class="text-[13px] font-semibold">{{ row.label }}</span>
                <i class="fas fa-chevron-down text-[10px] opacity-60 transition-transform" />
              </summary>
              <dl class="space-y-2 border-t border-primary/15 px-3 py-3">
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
            </details>
          </div>
        </details>
      </div>
    </div>
  </section>
</template>

<style scoped>
.desktop-sync-table {
  display: none;
}

.mobile-sync-list {
  display: flex;
  flex-direction: column;
}

.sync-comparison[open] > summary i,
.mode-card[open] > summary i {
  transform: rotate(180deg);
}

@media (min-width: 1280px) {
  .desktop-sync-table {
    display: block;
  }

  .mobile-sync-list {
    display: none;
  }
}
</style>
